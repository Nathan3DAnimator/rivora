id: abilities
kind: recipe
title: Habilidades e efeitos: feitiço com mana e recarga, buffs, debuffs, veneno | Abilities and effects: a spell with mana and cooldown, buffs, debuffs, poison | Habilidades y efectos: un hechizo con maná y enfriamiento, buffs, debuffs, veneno
keys: habilidade, habilidades, ability, abilities, gameplay ability, gameplay ability system, gas, feitico, feitiço, spell, magia, magic, skill, poder, power, mana, stamina, estamina, energia, custo, cost, recarga, cooldown, tempo de recarga, cargas, charges, conjurar, cast, cast time, tempo de conjuracao, canalizar, channel, efeito, efeitos, effect, effects, gameplay effect, buff, buffs, debuff, debuffs, veneno, poison, dano por segundo, damage over time, dot, cura por segundo, heal over time, regeneracao, regeneração, regeneration, mana regen, atributo, atributos, attribute, attributes, velocidade, speed, correr mais rapido, haste, lentidao, slow, defesa, defense, armadura, armor, dano, damage, atordoar, atordoado, stun, stunned, empilhar, acumulo, acúmulo, stack, stacks, stacking, modificador, modifier, multiplicar, give ability, remove ability, try activate ability, try activate ability by tag, cancel ability, end ability, get ability owner, is ability on cooldown, get cooldown remaining, is ability active, apply gameplay effect, remove gameplay effect, has gameplay effect, get attribute, set attribute, on activate ability, on ability activated, on ability ended, on effect applied, on effect removed, on attribute changed, ability system, wbp_abilitybar, barra de habilidades, ability bar, hotbar, teclas 1 a 6
--- pt
O **Gameplay Ability System** da Unreal numa versão mais simples, sobre as **Gameplay Tags**:
1. **Habilidade** (Content Browser > Adicionar > **Habilidade de Jogo**): uma classe de Grafo com pai **Gameplay Ability**. Nos padrões da classe (variáveis da categoria Ability): **Display Name**, **Icon**, **Cost Attribute** + **Cost** (`Mana` 30), **Cooldown**, **Charges** (usos antes de esperar; volta uma por recarga), **Cast Time**, **Ability Tags** (o que ela é: `Ability.Attack.Fire`), **Activation Owned Tags** (o dono tem enquanto roda), **Blocked By Tags** (`State.Stunned` impede), **Cancel Abilities With Tags** e **Self Effect** (um efeito no dono ao ativar). O grafo: **Event On Activate Ability** (Owner) faz a habilidade e **End Ability** termina.
2. **Efeito** (Adicionar > **Efeito de Jogo**, um Data Asset): **Duration Policy** (`Instant` muda o valor base uma vez; `Has Duration` ou `Infinite` mudam o valor atual enquanto ativos), **Duration**, **Period** (a cada X s: **Damage** / **Heal** pelo componente Vida e os modificadores no valor base, como uma regeneração), **Modifiers** (`Speed x1.5, Defense +10, Mana =50`), **Max Stacks** (0: cada um separado; N: até N acúmulos) com **Refresh Duration**, **Granted Tags** e **Required Tags**. Buffs e debuffs (Is Debuff: vermelho na lista).
3. No ator: **Details > Adicionar > Sistema de Habilidades**: **Atributos Iniciais** (`Mana 100, MaxMana 100`), **Habilidades Iniciais** (teclas 1-6 do jogador), **Efeitos Iniciais**, **Mostrar Barra de Habilidades**. **Speed** é a velocidade do Character; **Damage** multiplica o dano que ele causa; **Defense** reduz o que ele leva (x 100 / (100 + Defense)); `X` fica entre 0 e `MaxX`.
4. Nós: **Give Ability**, **Remove Ability**, **Try Activate Ability** (Failure Reason: Cooldown, Cost, Blocked...), **Try Activate Ability by Tag**, **Cancel Ability**, **End Ability**, **Get Ability Owner**, **Is Ability On Cooldown**, **Get Cooldown Remaining**, **Apply Gameplay Effect** (Target e Source), **Remove Gameplay Effect** (nome ou tag: uma limpeza), **Has Gameplay Effect**, **Get / Set Attribute**. Eventos no dono: **On Ability Activated**, **On Ability Ended**, **On Effect Applied**, **On Effect Removed**, **On Attribute Changed**.
5. **WBP_AbilityBar** (Content/UI): os espaços com ícone, recarga em círculo, cargas e tecla (clique ativa) e a lista dos efeitos ativos com o tempo. O **Save Progress** guarda atributos, efeitos com o tempo e recargas.

```diagram steps
Try Activate Ability | tags que bloqueiam? recarga? custo? | paga o custo, gasta uma carga | Cast Time | Event On Activate Ability | Apply Gameplay Effect no alvo | End Ability
```
```blueprint
Event On Activate Ability ( Owner ) -> Apply Gameplay Effect ( Target = Enemy, Effect = "GE_Poison", Source = Owner ) -> End Ability
Event Key 1 -> Try Activate Ability ( Ability = "GA_Fireball" ) -> Print String ( Failure Reason )
Event On Attribute Changed ( Attribute, New Value, Old Value ) -> Print String ( Format Text ( "{0}: {1}", Attribute, New Value ) )
```
```real
class GA_Heal extends GameplayAbility
    var CostAttribute: string = "Mana"
    var Cost: float = 20
    var Cooldown: float = 5

    event ActivateAbility(owner: Actor)
        owner.Heal(30)
        EndAbility()

class Hero extends Character
    event BeginPlay()
        GiveAbility("GA_Heal")
        SetAttribute("Mana", 100)

    event OnAbilityActivated(ability: string)
        print("usou " + ability)

    event OnEffectApplied(effect: string, stacks: int, source: Actor)
        print(effect + " x" + stacks)
```

O custo: 1000 atores com 5 efeitos cada custam ~0,02 ms por quadro (tudo em arrays fixos, sem alocar); a lista **Abilities and Effects** do Debug Viewer (F10) mostra o tempo e cada sistema. Veja também [Vida e Dano](article:health-damage) e [Gameplay Tags](article:gameplay-tags).
--- en
Unreal's **Gameplay Ability System** in a simpler version, on the **Gameplay Tags**:
1. **Ability** (Content Browser > Add > **Gameplay Ability**): a Graph class whose parent is **Gameplay Ability**. In its class defaults (the variables of the category Ability): **Display Name**, **Icon**, **Cost Attribute** + **Cost** (`Mana` 30), **Cooldown**, **Charges** (uses before it waits; one comes back each cooldown), **Cast Time**, **Ability Tags** (what it is: `Ability.Attack.Fire`), **Activation Owned Tags** (the owner has them while it runs), **Blocked By Tags** (`State.Stunned` stops it), **Cancel Abilities With Tags** and **Self Effect** (an effect on the owner when it activates). Its graph: **Event On Activate Ability** (Owner) does it and **End Ability** ends it.
2. **Effect** (Add > **Gameplay Effect**, a Data Asset): **Duration Policy** (`Instant` changes the base value once; `Has Duration` or `Infinite` change the current value while active), **Duration**, **Period** (every X s: **Damage** / **Heal** through the Health component and the modifiers onto the base, like a regeneration), **Modifiers** (`Speed x1.5, Defense +10, Mana =50`), **Max Stacks** (0: each one apart; N: up to N stacks) with **Refresh Duration**, **Granted Tags** and **Required Tags**. Buffs and debuffs (Is Debuff: red in the list).
3. On the actor: **Details > Add > Ability System**: **Starting Attributes** (`Mana 100, MaxMana 100`), **Starting Abilities** (the player's keys 1-6), **Starting Effects**, **Show Ability Bar**. **Speed** is the Character's speed; **Damage** multiplies the damage it deals; **Defense** reduces what it takes (x 100 / (100 + Defense)); `X` stays within 0 and `MaxX`.
4. Nodes: **Give Ability**, **Remove Ability**, **Try Activate Ability** (Failure Reason: Cooldown, Cost, Blocked...), **Try Activate Ability by Tag**, **Cancel Ability**, **End Ability**, **Get Ability Owner**, **Is Ability On Cooldown**, **Get Cooldown Remaining**, **Apply Gameplay Effect** (Target and Source), **Remove Gameplay Effect** (a name or a tag: a cleanse), **Has Gameplay Effect**, **Get / Set Attribute**. Events on the owner: **On Ability Activated**, **On Ability Ended**, **On Effect Applied**, **On Effect Removed**, **On Attribute Changed**.
5. **WBP_AbilityBar** (Content/UI): the slots with the icon, the cooldown as a sweep, the charges and the key (a click activates) and the list of the active effects with their time. **Save Progress** keeps the attributes, the effects with their time and the cooldowns.

```diagram steps
Try Activate Ability | blocking tags? cooldown? cost? | the cost paid, a charge spent | Cast Time | Event On Activate Ability | Apply Gameplay Effect on the target | End Ability
```
```blueprint
Event On Activate Ability ( Owner ) -> Apply Gameplay Effect ( Target = Enemy, Effect = "GE_Poison", Source = Owner ) -> End Ability
Event Key 1 -> Try Activate Ability ( Ability = "GA_Fireball" ) -> Print String ( Failure Reason )
Event On Attribute Changed ( Attribute, New Value, Old Value ) -> Print String ( Format Text ( "{0}: {1}", Attribute, New Value ) )
```
```real
class GA_Heal extends GameplayAbility
    var CostAttribute: string = "Mana"
    var Cost: float = 20
    var Cooldown: float = 5

    event ActivateAbility(owner: Actor)
        owner.Heal(30)
        EndAbility()

class Hero extends Character
    event BeginPlay()
        GiveAbility("GA_Heal")
        SetAttribute("Mana", 100)

    event OnAbilityActivated(ability: string)
        print("used " + ability)

    event OnEffectApplied(effect: string, stacks: int, source: Actor)
        print(effect + " x" + stacks)
```

The cost: 1000 actors with 5 effects each take ~0.02 ms a frame (everything in fixed arrays, nothing allocated); the Debug Viewer's (F10) **Abilities and Effects** list shows the time and each system. See also [Health and Damage](article:health-damage) and [Gameplay Tags](article:gameplay-tags).
--- es
El **Gameplay Ability System** de Unreal en una versión más simple, sobre las **Gameplay Tags**:
1. **Habilidad** (Explorador de contenidos > Añadir > **Gameplay Ability**): una clase de Grafo cuyo padre es **Gameplay Ability**. En los valores por defecto de la clase (las variables de la categoría Ability): **Display Name**, **Icon**, **Cost Attribute** + **Cost** (`Mana` 30), **Cooldown**, **Charges** (usos antes de esperar; vuelve una en cada enfriamiento), **Cast Time**, **Ability Tags** (lo que es: `Ability.Attack.Fire`), **Activation Owned Tags** (el dueño las tiene mientras se ejecuta), **Blocked By Tags** (`State.Stunned` la detiene), **Cancel Abilities With Tags** y **Self Effect** (un efecto sobre el dueño al activarse). Su grafo: **Event On Activate Ability** (Owner) la ejecuta y **End Ability** la termina.
2. **Efecto** (Añadir > **Gameplay Effect**, un Data Asset): **Duration Policy** (`Instant` cambia el valor base una vez; `Has Duration` o `Infinite` cambian el valor actual mientras están activos), **Duration**, **Period** (cada X s: **Damage** / **Heal** a través del componente de Salud y los modificadores sobre el valor base, como una regeneración), **Modifiers** (`Speed x1.5, Defense +10, Mana =50`), **Max Stacks** (0: cada uno por separado; N: hasta N acumulaciones) con **Refresh Duration**, **Granted Tags** y **Required Tags**. Buffs y debuffs (Is Debuff: rojo en la lista).
3. En el actor: **Detalles > Añadir > Ability System**: **Starting Attributes** (`Mana 100, MaxMana 100`), **Starting Abilities** (las teclas 1-6 del jugador), **Starting Effects**, **Show Ability Bar**. **Speed** es la velocidad del Character; **Damage** multiplica el daño que causa; **Defense** reduce lo que recibe (x 100 / (100 + Defense)); `X` se mantiene entre 0 y `MaxX`.
4. Nodos: **Give Ability**, **Remove Ability**, **Try Activate Ability** (Failure Reason: Cooldown, Cost, Blocked...), **Try Activate Ability by Tag**, **Cancel Ability**, **End Ability**, **Get Ability Owner**, **Is Ability On Cooldown**, **Get Cooldown Remaining**, **Apply Gameplay Effect** (Target y Source), **Remove Gameplay Effect** (un nombre o una etiqueta: una purga), **Has Gameplay Effect**, **Get / Set Attribute**. Eventos en el dueño: **On Ability Activated**, **On Ability Ended**, **On Effect Applied**, **On Effect Removed**, **On Attribute Changed**.
5. **WBP_AbilityBar** (Content/UI): las casillas con el ícono, el enfriamiento como barrido, las cargas y la tecla (un clic activa) y la lista de los efectos activos con su tiempo. **Save Progress** guarda los atributos, los efectos con su tiempo y los enfriamientos.

```diagram steps
Try Activate Ability | ¿etiquetas de bloqueo? ¿enfriamiento? ¿costo? | el costo se paga, se gasta una carga | Cast Time | Event On Activate Ability | Apply Gameplay Effect en el objetivo | End Ability
```
```blueprint
Event On Activate Ability ( Owner ) -> Apply Gameplay Effect ( Target = Enemy, Effect = "GE_Poison", Source = Owner ) -> End Ability
Event Key 1 -> Try Activate Ability ( Ability = "GA_Fireball" ) -> Print String ( Failure Reason )
Event On Attribute Changed ( Attribute, New Value, Old Value ) -> Print String ( Format Text ( "{0}: {1}", Attribute, New Value ) )
```
```real
class GA_Heal extends GameplayAbility
    var CostAttribute: string = "Mana"
    var Cost: float = 20
    var Cooldown: float = 5

    event ActivateAbility(owner: Actor)
        owner.Heal(30)
        EndAbility()

class Hero extends Character
    event BeginPlay()
        GiveAbility("GA_Heal")
        SetAttribute("Mana", 100)

    event OnAbilityActivated(ability: string)
        print("used " + ability)

    event OnEffectApplied(effect: string, stacks: int, source: Actor)
        print(effect + " x" + stacks)
```

El costo: 1000 actores con 5 efectos cada uno toman ~0.02 ms por cuadro (todo en arreglos fijos, nada se asigna); la lista **Abilities and Effects** del Debug Viewer (F10) muestra el tiempo y cada sistema. Consulta también [Salud y daño](article:health-damage) y [Gameplay Tags](article:gameplay-tags).
