# Saúde e Dano (módulo 1 do R7)

Pedido (plano de melhorias, R7 módulo 1): um componente de vida modular e reutilizável, como os Health Components dos
jogos da Unreal, feito do zero (sem nada das demos de luta, terror ou RPG), ligável por Blueprint e scripts e
configurado no Details.

Código: `src/health.h/.c` (dados, regras, Debug Viewer, WBP_HealthBar, autoteste), componente `CHealth` em
`components.h`, nós em `bp_internal.h`/`blueprint.c`/`bp_vm.c` (os eventos chegam pelo `health_event` do `bp_vm.c`),
números flutuantes em `ui_widget_draw.c` (`ui_draw_damage_numbers`), Details em `editor.c` e `bp_editor.c`, eventos dos
scripts em `script_lower.c`, teste `src/health_test.c`.

## Como funciona
- **Componente Vida** (Details > Adicionar > Vida; no Blueprint, Componentes > + Adicionar > Jogabilidade, como
  `HealthComponent`): Vida Máxima, Vida (a inicial; jogando, a atual com uma barra "Estado"), Escudo Máximo e Escudo,
  Atraso da Regeneração, Regeneração por Segundo (vida e escudo separados), Invencível, Números de Dano, Mostrar Barra
  de Vida, Ao Morrer (Só o Evento, Destruir o Ator, Ragdoll) com o atraso, e 3 Multiplicadores de Dano.
- **Tipo de dano = Gameplay Tag** (`Damage.Fire`...): o multiplicador mais específico que casa com o tipo vale
  (`gt_matches`, hierarquia por pontos): com `Damage` x0,5 e `Damage.Fire` x2, `Damage.Fire.Burning` leva x2 e
  `Damage.Physical` x0,5. Sem tipo ou sem multiplicador: x1. x0: imune.
- **Dano**: quantidade x multiplicador; o escudo absorve primeiro, o resto vai para a vida. Morto ou Invencível: nada
  (nem evento). O dano devolvido e o do evento são o que o ator **perdeu** (escudo + vida), não o excesso.
- **Regeneração**: depois de `regen_delay` segundos sem dano, vida e escudo sobem nas suas taxas. Não dispara On
  Healed (seria um evento por quadro).
- **Morte** (vida 0): Event On Death (Killer = o instigador, tipo, causador); depois, por Ao Morrer: só isso; o ator
  destruído após o atraso (com o End Play dele, como o Destroy Actor); ou o ragdoll da Skeletal Mesh (`phys_set_simulate`,
  precisa de Physics Asset) e, com atraso > 0, destruído depois. **Revive** volta (vida dada ou cheia, escudo cheio,
  ragdoll desligado, destruição cancelada). Morto não leva dano nem cura.
- **Eventos** no próprio ator: Event AnyDamage (Damage, Damage Type, Instigated By, Damage Causer: os nomes da Unreal;
  "On Damaged" na descrição), Event On Healed (Amount, Healer), Event On Death (Killer, Damage Type, Damage Causer),
  Event On Shield Broken (Instigated By). A ordem: AnyDamage, Shield Broken, Death (Death só se continua morto depois
  do AnyDamage: um AnyDamage que cura ou revive cancela).
- Os eventos rodam na hora (dentro do Apply Damage), como na Unreal; o dano radial junta os atores no alcance antes
  (os eventos podem mexer nos arquétipos) e as destruições ficam para o fim do `health_update` (no `bp_tick`, depois
  dos timers). As ações não rodam no Construction Script do editor.
- **Sem malloc no quadro**: tudo no componente (120 bytes) e em arrays fixos (256 destruições por quadro, 2048 atores
  por dano radial, 64 números).

## Nós (categoria Game > Damage)
Apply Damage (Target, Amount, Damage Type, Instigator, Damage Causer → o dano levado), Apply Radial Damage (Base
Damage, Origin, Damage Radius, Damage Type, Instigator, Damage Causer, Falloff, Minimum Damage → Return Value, Hit
Count; dano = mínimo + (base − mínimo) × (1 − d/raio)^Falloff, Falloff 0 = cheio em todo o raio, como o Apply Radial
Damage with Falloff), Heal, Get Health, Get Health Percent, Get Max Health, Get Shield (+ Percent, Max Shield), Set
Max Health (Fill Health), Set Shield, Is Dead, Revive, Set Invincible, Set Damage Multiplier. Target vazio = o próprio
ator. Nos scripts entram sozinhos (`ApplyDamage`, `other.ApplyDamage(...)`...); os eventos são os métodos
`OnAnyDamage`/`OnDamaged`/`OnTakeDamage`/`ReceiveAnyDamage`, `OnHealed`, `OnDeath`/`OnDied`, `OnShieldBroken` (só os
nomes com On: um `TakeDamage` do usuário continua função).

## Widget de exemplo e números
- **WBP_HealthBar** (`health_bar_asset`, em `Content/UI`, feito quando falta: ao adicionar a Vida no Details ou no
  Blueprint, e ao começar o jogo com Mostrar Barra de Vida): moldura escura no canto inferior esquerdo, barra de escudo
  fina (some sem escudo), barra de vida e o texto "60 / 80". O grafo é o de um Widget Blueprint comum (Event Tick → Get
  Player Character → Get Health Percent / Get Shield / Format Text → Set Percent / Set Widget Visible / Set Text): o
  usuário abre, muda e usa no próprio jogo com Create Widget + Add to Viewport.
- **Mostrar Barra de Vida** no pawn do jogador cria e mostra o widget no BeginPlay (depois dos BeginPlay do nível).
- **Números de Dano**: amarelo na vida, azul no escudo, verde "+N" na cura; sobem 70 cm em 1,1 s e somem, com um pulo
  ao aparecer, tamanho pela distância. Desenhados por cima da vista no PIE e no jogo empacotado (o jogo abre o quadro
  do ImGui só enquanto há número ou widget).

## Formato
- `BpAsset.comp_data` foi de 96 para **128 bytes** por componente (o `CHealth` tem 120): o `.rbp` grava o hex do
  tamanho novo; os antigos (96) abrem iguais (o resto zero). O Desfazer do Details guarda 128 bytes por componente:
  o `CHealth` cabe.
- `COMP_HEALTH` no fim do enum; níveis antigos não têm.

## Medidas (RTX 3060, 2560x1417, Release)
| Caso | Resultado |
|---|---|
| 1000 atores com Vida (escudo, regeneração, multiplicador) levando dano todo quadro, 600 quadros | **0,106–0,111 ms/quadro** (dano + update); update sozinho 0,006 ms |
| Apply Radial Damage sobre 725 atores (de 1000) | 0,05 ms |
| Cena sem nenhum componente Vida | o `health_update` só percorre os arquétipos (< 1 µs) |
| Teste `health` (editor com validação, 4 atores com Vida, jogador com a barra) | 525–651 fps |
O Debug Viewer (F10) tem a lista **Health and Damage**: update + dano do quadro (o dano só é cronometrado com o viewer
aberto), quantos componentes, mortos, danos, curas, mortes, e cada ator com a vida (custo repartido, ≈).

## Teste `health` (`--health-test`)
1. `health_selftest`: tipos e multiplicador (o mais específico, imune, três no máximo, trocar um), escudo (absorve,
   quebra uma vez), regeneração (espera o atraso, taxas, um dano novo espera de novo), cura (limitada), morte com o
   evento (killer e tipo), morto não leva nada, revive, Set Max Health (manter, limitar, encher), destruir após o
   atraso, invencível, radial (queda linear, Falloff 0, Falloff 2 com mínimo), 1000 atores por quadro (orçamento 1 ms).
2. Jogando: BP_HealthTarget (Vida nos padrões da classe: escudo 20, Damage.Fire x2) se acerta no BeginPlay e se cura;
   BP_HealthBomber explode (radial 600 cm) e depois revive o alvo; uma caixa com Destruir o Ator some; uma parede
   invencível fica inteira; o jogador com Mostrar Barra de Vida vê 0,75 e "60 / 80". Confere os quatro eventos, o
   Killer, o Hit Count, Get Health Percent, Is Dead, os números.

## Limites / próximos passos
- O dano radial mede até o centro do ator e não é bloqueado por paredes (a Unreal tem o canal de prevenção).
- Os eventos chegam ao Blueprint do próprio ator; Level Blueprint e Actor Component Blueprints ainda não.
- No ragdoll o Character continua com o movimento (desligue a entrada no On Death).
- Online: a vida não é replicada sozinha; o servidor aplica o dano e replica o que o jogo precisar (variável
  replicada, RPC).
- Barra sobre a cabeça de inimigos (widget no mundo) fica para quando houver Widget Component.

## Números unificados com o HUD (R7 módulo 10)
Os números ficam no mesmo buffer (`health_push_number`, também do nó Show Damage Number: crítico e cura) e são desenhados pelo widget **Números de Dano** do HUD nas cores dele; sem esse widget na tela, `ui_draw_damage_numbers` desenha como antes. Cada dano avisa o HUD (`hud_on_damage`: a Direção do Dano). Ver [hud.md](hud.md).
