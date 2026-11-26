id: motion-matching-jumps
kind: recipe
title: Pulos no Motion Matching (root motion vertical) | Jumps in Motion Matching (vertical root motion) | Saltos en Motion Matching (root motion vertical)
keys: motion matching, pulo, pular, jump, take longo, long take, root motion vertical, vertical root motion, altura da raiz, root height, pulo da animacao, animation jump, mocap, decolagem, takeoff, aterrissagem, landing, teto, ceiling, jumptake, abp_jumptake, mm_jumptake
--- pt
Um take longo (ou um clipe) com **pulos** dentro serve ao **Motion Matching**: a altura do pulo é **root motion vertical**. Ela sai da pose e passa a mover a **cápsula** do Character, que sobe e desce como na animação (e colide). Sem Jump, ele nunca escolhe um pulo sozinho.
- **Como ele acha o pulo**: a altura da **raiz** (osso raiz no chão, como na Unreal). Se a raiz fica parada (mocap) ou está no quadril, ele usa o **arco da pélvis** enquanto os dois pés estão fora do chão. Um "voo" de corrida com menos de 10 cm continua na pose.
- **Jump**: com um banco que tem pulos, o nó **Jump** pede a **decolagem** da animação (até 0,25 s). Se nenhuma servir, ele faz o pulo físico (Jump Z Velocity).
- **Durante o pulo** o clipe não troca e roda em tempo real. **Is Falling** fica falso, então o estado Jump do Anim Graph não interrompe.
- **Fim**: aterrissou (no chão que a animação espera ou antes, num degrau); ou **bloqueou** (teto, beirada: 20 cm fora da altura do clipe), e aí a gravidade continua da velocidade do clipe.
- **Ver**: Editor de Motion Matching > Depurador mostra "Pulo: X cm/s para cima" nos quadros de pulo. O exemplo **JumpTake** (`MM_JumpTake` + `ABP_JumpTake`) pula parado, andando e correndo.
- **Custo**: duas features a mais por quadro do banco; a busca continua em ~0,01 ms.

```diagram steps
Take com pulos | Motion Matching (Locomotion) | Jump | decolagem do take | a cápsula segue o clipe | aterrissa
```
```blueprint
Event Key Space -> Jump
```
--- en
A long take (or a clip) with **jumps** in it works for **Motion Matching**: the jump's height is **vertical root motion**. It is taken out of the pose and moves the Character's **capsule**, which rises and falls as in the animation (and collides). Without Jump it never picks a jump by itself.
- **How it finds the jump**: the **root**'s height (a root bone on the floor, as in Unreal). If the root stays still (mocap) or sits at the hips, it uses the **pelvis's arc** while both feet are off the ground. A run's float under 10 cm stays in the pose.
- **Jump**: with a database that has jumps, the **Jump** node asks the animation for a **takeoff** (up to 0.25 s). If none fits, it makes the physical jump (Jump Z Velocity).
- **During the jump** the clip does not switch and plays in real time. **Is Falling** stays false, so the Anim Graph's Jump state does not cut in.
- **End**: it landed (on the floor the animation expects, or sooner, on a step); or it was **blocked** (a ceiling, a ledge: 20 cm off the clip's height), and gravity goes on from the clip's velocity.
- **See it**: Motion Matching Editor > Query Debugger shows "Jump: X cm/s up" on jump frames. The **JumpTake** example (`MM_JumpTake` + `ABP_JumpTake`) jumps standing, walking and running.
- **Cost**: two more features per database frame; the search stays at ~0.01 ms.

```diagram steps
Take with jumps | Motion Matching (Locomotion) | Jump | the take's takeoff | the capsule follows the clip | lands
```
```blueprint
Event Key Space -> Jump
```
--- es
Un take largo (o un clip) con **saltos** funciona para **Motion Matching**: la altura del salto es **root motion vertical**. Se saca de la pose y mueve la **cápsula** del Character, que sube y baja como en la animación (y colisiona). Sin Jump nunca elige un salto por sí solo.
- **Cómo encuentra el salto**: la altura del **root** (un hueso raíz en el suelo, como en Unreal). Si el root se queda quieto (mocap) o está en las caderas, usa el **arco de la pelvis** mientras ambos pies están fuera del suelo. La flotación de una carrera por debajo de 10 cm se queda en la pose.
- **Jump**: con una base de datos que tiene saltos, el nodo **Jump** le pide a la animación un **despegue** (hasta 0,25 s). Si ninguno encaja, hace el salto físico (Jump Z Velocity).
- **Durante el salto** el clip no cambia y se reproduce en tiempo real. **Is Falling** queda en falso, así que el estado Jump del Anim Graph no interviene.
- **Fin**: aterrizó (en el suelo que la animación espera, o antes, en un escalón); o fue **bloqueado** (un techo, un saliente: 20 cm por encima de la altura del clip), y la gravedad sigue desde la velocidad del clip.
- **Verlo**: Editor de Motion Matching > Query Debugger muestra "Jump: X cm/s up" en los cuadros de salto. El ejemplo **JumpTake** (`MM_JumpTake` + `ABP_JumpTake`) salta parado, caminando y corriendo.
- **Costo**: dos características más por cuadro de la base de datos; la búsqueda se queda en ~0,01 ms.

```diagram steps
Take con saltos | Motion Matching (Locomotion) | Jump | el despegue del take | la cápsula sigue al clip | aterriza
```
```blueprint
Event Key Space -> Jump
```
