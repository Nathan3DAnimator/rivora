id: traversal
kind: recipe
title: Traversal: pular, subir e escalar obstáculos | Traversal: vault, mantle and climb obstacles | Traversal: saltar, trepar y escalar obstáculos
keys: traversal, travessia, parkour, pular muro, vault, vault over, pular obstaculo, jump over obstacle, subir no muro, mantle, climb up, escalar, climb, subir caixa, climb a box, borda, ledge, agarrar borda, grab ledge, try traversal, game animation sample, pular cerca, hop a fence, subir parede, climb a wall
--- pt
```diagram steps
Try Traversal | traces: frente, topo, profundidade, chão atrás, espaço | Vault, Mantle ou Climb | alvos FrontLedge e o chão | montage com root motion e warp
```
## O que faz
- **Try Traversal** (Target, Max Distance, Max Height, Draw Debug) olha o que está à frente do Character e escolhe sozinho:
  - **Vault**: obstáculo baixo (até 130 cm) e fino (até 90 cm), com chão mais baixo atrás: passa por cima e cai do outro lado.
  - **Mantle**: obstáculo baixo e fundo: sobe nele.
  - **Climb**: obstáculo alto (até Max Height, padrão 230 cm): pula, agarra a borda, puxa o corpo e sobe.
  - **None**: degrau (menos de 30 cm), alto demais ou sem espaço para o corpo. Return Value falso.
- As **mãos caem na borda** e os **pés no chão** de qualquer altura e profundidade, pelo **Motion Warping** (alvos `FrontLedge` e `BackFloor` / `TopFloor`). Durante a ação o corpo atravessa o obstáculo (**Ignorar Colisão** no montage), como o modo Flying da Unreal.
- **Draw Debug** mostra os traces por 2 s: vermelho onde bateu, verde livre, amarelo as bordas, a cápsula onde vai terminar.
- O template **Terceira Pessoa** já usa: **Espaço** tenta o traversal e pula quando não há nada.
- Clipes e montages prontos em `Characters/Mannequin/Traversal` (`AM_Traversal_Vault`, `_Mantle`, `_Climb`); troque o clipe do montage para usar os seus.
- Custo: 0,044 ms por checagem (cerca de 15 traces).

```blueprint
Event Key Space -> Try Traversal ( Target = self, Max Distance = 150, Max Height = 230, Draw Debug = false )
  -> Branch ( Return Value ) False -> Jump
```
--- en
```diagram steps
Try Traversal | traces: front, top, depth, floor behind, room | Vault, Mantle or Climb | FrontLedge and floor targets | montage with root motion and warp
```
## What it does
- **Try Traversal** (Target, Max Distance, Max Height, Draw Debug) looks at what is ahead of the Character and picks by itself:
  - **Vault**: a low (up to 130 cm), thin (up to 90 cm) obstacle with lower floor behind it: over it, landing on the other side.
  - **Mantle**: a low, deep obstacle: up onto it.
  - **Climb**: a high obstacle (up to Max Height, 230 cm by default): jumps, grabs the ledge, pulls up and stands on it.
  - **None**: a step (under 30 cm), too high, or no room for the body. Return Value false.
- The **hands land on the ledge** and the **feet on the floor** at any height and depth, by **Motion Warping** (targets `FrontLedge` and `BackFloor` / `TopFloor`). While it plays the body passes through the obstacle (**Ignore Collision** in the montage), like Unreal's Flying mode.
- **Draw Debug** shows the traces for 2 s: red where they hit, green free, yellow the edges, the capsule where it will end.
- The **Third Person** template already uses it: **Space** tries the traversal and jumps when there's nothing.
- Ready clips and montages in `Characters/Mannequin/Traversal` (`AM_Traversal_Vault`, `_Mantle`, `_Climb`); change the montage's clip to use your own.
- Cost: 0.044 ms a check (about 15 traces).

```blueprint
Event Key Space -> Try Traversal ( Target = self, Max Distance = 150, Max Height = 230, Draw Debug = false )
  -> Branch ( Return Value ) False -> Jump
```
--- es
```diagram steps
Try Traversal | trazados: frente, parte superior, profundidad, suelo detrás, espacio | Vault, Mantle o Climb | objetivos FrontLedge y suelo | montage con root motion y warp
```
## Qué hace
- **Try Traversal** (Target, Max Distance, Max Height, Draw Debug) mira lo que hay delante del Character y elige por sí solo:
  - **Vault**: un obstáculo bajo (hasta 130 cm) y delgado (hasta 90 cm) con suelo más bajo detrás: pasa por encima y aterriza al otro lado.
  - **Mantle**: un obstáculo bajo y profundo: sube encima.
  - **Climb**: un obstáculo alto (hasta Max Height, 230 cm de forma predeterminada): salta, se agarra al borde, se impulsa y se pone de pie encima.
  - **None**: un escalón (menos de 30 cm), demasiado alto, o sin espacio para el cuerpo. Return Value false.
- Las **manos caen sobre el borde** y los **pies sobre el suelo** a cualquier altura y profundidad, mediante **Motion Warping** (objetivos `FrontLedge` y `BackFloor` / `TopFloor`). Mientras se reproduce, el cuerpo atraviesa el obstáculo (**Ignore Collision** en el montage), como el modo Flying de Unreal.
- **Draw Debug** muestra los trazados durante 2 s: en rojo donde chocan, en verde libres, en amarillo los bordes, la cápsula donde terminará.
- La plantilla **Third Person** ya lo usa: **Space** prueba el traversal y salta cuando no hay nada.
- Clips y montages listos en `Characters/Mannequin/Traversal` (`AM_Traversal_Vault`, `_Mantle`, `_Climb`); cambia el clip del montage para usar el tuyo.
- Costo: 0,044 ms por comprobación (unos 15 trazados).

```blueprint
Event Key Space -> Try Traversal ( Target = self, Max Distance = 150, Max Height = 230, Draw Debug = false )
  -> Branch ( Return Value ) False -> Jump
```
