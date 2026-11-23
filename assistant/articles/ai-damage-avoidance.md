id: ai-damage-avoidance
kind: recipe
title: IA que sente o dano e desvia dos outros | AI that feels damage and avoids the others
keys: sentido de dano, damage sense, aisense damage, report damage event, inimigo nao reage quando atingido, ia nao percebe quem acertou, ia atingida de surpresa, enemy doesn't react when shot, ai ignores damage, quem me acertou, desvio, avoidance, rvo, rvo avoidance, inimigos atravessam um ao outro, ia atravessa outra ia, personagens se atravessam, agents walk through each other, characters walk through each other, enemies pass through each other, npcs overlap, local avoidance, desviar de outro personagem, multidao pequena, empurrar
--- pt
Como o **AISense_Damage** e o **RVO Avoidance** da Unreal, já ligados em todo agente de IA (componente **IA**):
1. **Sentido de dano**: um **Apply Damage** (ou **Apply Radial Damage**) num ator com o componente **IA** e **Vida** conta para a percepção. Ele sabe na hora quem bateu (o **Instigator**), de qualquer lado, sem cone e sem tempo de reação: `TargetActor` vira quem atirou, `LastKnownLocation` e `NoiseLocation` o lugar dele, `HeardNoise` liga por 4 s. Parado, ele **vira para o atirador** (2 s) e, ao vê-lo, `CanSeeTarget` liga: a Behavior Tree e o Real Brain reagem como se o tivessem visto.
2. Não conta: o dano de alguém do **mesmo Time** (fogo amigo) e um agente de **Time 0** (ignora os outros).
3. **Desvio entre agentes**: andando (AI Move To, Move To da árvore, Chase, Patrol...), um agente que vai cruzar outro Character ou o jogador nos próximos 1,2 s se desvia para o lado; de frente, cada um vai para a sua direita, como gente na calçada. Encostados, se afastam. O alvo e o ator para onde ele vai não contam (ele quer chegar neles).
4. Custo: uma grade de hash por quadro, sem alocar; a lista **AI** do Debug Viewer (F10) mostra o tempo da IA. `REALENGINE_AI_AVOID=0` desliga o desvio (para comparar).

```diagram steps
Apply Damage no inimigo (Instigator = quem atirou) | O inimigo sabe quem foi: TargetActor | Vira para o atirador e o vê: CanSeeTarget | Persegue desviando dos outros
```
```blueprint
Event ActorBeginOverlap -> Apply Damage ( Target = Other Actor, Amount = 25, Damage Type = "Damage.Physical", Instigator = self )
```
```real
class Bullet extends Actor
    event OnBeginOverlap(other: Actor)
        other.ApplyDamage(25.0, "Damage.Physical", self, self)
        DestroyActor()
```

O **Instigator** é quem a IA vai caçar: passe o pawn de quem atirou (numa bala, o dono dela). Para o resto da percepção (visão, audição, esquadrões) veja [IA a fundo](article:ai-pro) e [NavMesh](article:navmesh).
--- en
Like Unreal's **AISense_Damage** and **RVO Avoidance**, already on in every AI agent (the **AI** component):
1. **Damage sense**: an **Apply Damage** (or **Apply Radial Damage**) on an actor with the **AI** and **Health** components counts for its perception. It knows at once who hit it (the **Instigator**), from any side, no cone and no reaction time: `TargetActor` becomes the shooter, `LastKnownLocation` and `NoiseLocation` its place, `HeardNoise` is on for 4 s. Standing, it **turns to the shooter** (2 s) and, seeing it, `CanSeeTarget` goes on: the Behavior Tree and the Real Brain react as if it had seen it.
2. Not counted: damage from someone on the **same Team** (friendly fire) and an agent of **Team 0** (it ignores the others).
3. **Avoidance between agents**: walking (AI Move To, the tree's Move To, Chase, Patrol...), an agent about to cross another Character or the player within 1.2 s steps aside; head-on, each goes to its own right, like people on a sidewalk. Touching, they move apart. Its target and the actor it walks to don't count (it wants to reach them).
4. Cost: a hash grid a frame, nothing allocated; the Debug Viewer's **AI** list (F10) shows the AI's time. `REALENGINE_AI_AVOID=0` turns the avoidance off (to compare).

```diagram steps
Apply Damage on the enemy (Instigator = the shooter) | The enemy knows who: TargetActor | It turns to the shooter and sees it: CanSeeTarget | It chases, going round the others
```
```blueprint
Event ActorBeginOverlap -> Apply Damage ( Target = Other Actor, Amount = 25, Damage Type = "Damage.Physical", Instigator = self )
```
```real
class Bullet extends Actor
    event OnBeginOverlap(other: Actor)
        other.ApplyDamage(25.0, "Damage.Physical", self, self)
        DestroyActor()
```

The **Instigator** is who the AI will hunt: pass the shooter's pawn (for a bullet, its owner). For the rest of the perception (sight, hearing, squads) see [AI in depth](article:ai-pro) and [NavMesh](article:navmesh).
