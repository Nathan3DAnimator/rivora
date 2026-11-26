id: motion-warping
kind: recipe
title: Root Motion e Motion Warping: alinhar o personagem a um alvo | Root Motion and Motion Warping: align the character to a target | Root Motion y Motion Warping: alinear al personaje con un objetivo
keys: motion warping, warp target, alvo do warp, alinhar personagem, align character, alinhar ao alvo, align to target, root motion, movimento da raiz, montage com root motion, root motion montage, ataque que alcanca, attack that reaches, pular muro, vault, subir parede, climb, mantle, abrir porta, open door, maçaneta, door handle, janela de warp, warp window, add or update warp target, remove warp target, sincronizar posicao, match position
--- pt
```diagram steps
Add or Update Warp Target (lugar + direção) | Play Montage (com Root Motion) | Janela de Warp curva o caminho | Chega exato no alvo
```
## Root Motion
- No **Montage Editor**, marque **Root Motion**: o caminho da raiz do clipe move o **ator** (um Character anda com colisão e o input espera) e a malha fica nele, sem escorregar.
- Funciona com clipes que andam pela raiz (`root`) ou só pela pelve: a engine tira esse caminho da pose sozinha.
- No fim natural do clipe o personagem **para onde o clipe para** (sem deslizar na última velocidade). Um **Stop Montage** antes do fim apaga o movimento com o peso.

## Motion Warping (como o da Unreal)
- Em **Motion Warping**, **Add Warp Window**: o nome do alvo (`Target`), o início e o fim da janela (segundos do clipe) e o que corrigir: **Lugar**, **Direção**, **Ignorar Altura**.
- Na hora de tocar, **Add or Update Warp Target** (Target, Warp Target Name, Location, Rotation) diz onde a janela deve terminar. Dentro da janela o caminho do clipe se curva aos poucos, pelo próprio andar do clipe, e o ator termina **exatamente** no lugar e na direção do alvo.
- Usos: um ataque que alcança o inimigo onde ele estiver, pular um muro de qualquer altura (desmarque Ignorar Altura), a mão na maçaneta da porta, sentar na cadeira.
- **Remove Warp Target** esquece o alvo: a janela toca o clipe como ele é.

```blueprint
Event Key F -> Add or Update Warp Target ( Target = self, Warp Target Name = "Target", Location = Get Actor Location( Get Player Character ), Rotation = Get Actor Rotation( Get Player Character ) )
            -> Play Montage ( Target = self, Play Rate = 1 )
```
O montage (com Root Motion e uma janela no alvo `Target`) fica nos Detalhes do Play Montage. Custo: a animação inteira de dois personagens com root motion e warp fica em 0,012 ms por quadro (teste `warp`).
--- en
```diagram steps
Add or Update Warp Target (place + facing) | Play Montage (with Root Motion) | Warp window bends the path | Arrives exactly at the target
```
## Root Motion
- In the **Montage Editor**, tick **Root Motion**: the clip's root path moves the **actor** (a Character walks with collision and its input waits) and the mesh stays on it, no sliding.
- It works with clips that travel by the root (`root`) or only by the pelvis: the engine takes that path out of the pose itself.
- At the clip's natural end the character **stops where the clip stops** (no glide on its last speed). A **Stop Montage** before the end fades the motion with its weight.

## Motion Warping (like Unreal's)
- Under **Motion Warping**, **Add Warp Window**: the target's name (`Target`), the window's start and end (seconds of the clip) and what to correct: **Place**, **Facing**, **Ignore Height**.
- When playing, **Add or Update Warp Target** (Target, Warp Target Name, Location, Rotation) says where the window must end. Inside the window the clip's path bends little by little, by the clip's own travel, and the actor ends **exactly** at the target's place and facing.
- Uses: an attack that reaches the enemy wherever it is, vaulting a wall of any height (untick Ignore Height), the hand on a door handle, sitting on a chair.
- **Remove Warp Target** forgets the target: the window plays the clip as it is.

```blueprint
Event Key F -> Add or Update Warp Target ( Target = self, Warp Target Name = "Target", Location = Get Actor Location( Get Player Character ), Rotation = Get Actor Rotation( Get Player Character ) )
            -> Play Montage ( Target = self, Play Rate = 1 )
```
The montage (with Root Motion and a window on the target `Target`) goes in the Play Montage's Details. Cost: the whole animation of two characters with root motion and a warp stays at 0.012 ms a frame (the `warp` test).
--- es
```diagram steps
Add or Update Warp Target (lugar + orientación) | Play Montage (con Root Motion) | La ventana Warp curva el recorrido | Llega exactamente al objetivo
```
## Root Motion
- En el **Montage Editor**, marca **Root Motion**: el recorrido de la raíz del clip mueve al **actor** (un Character camina con colisión y su input espera) y la malla se queda sobre él, sin deslizarse.
- Funciona con clips que viajan por la raíz (`root`) o solo por la pelvis: el motor saca ese recorrido de la pose por sí mismo.
- Al final natural del clip el personaje **se detiene donde se detiene el clip** (sin deslizarse con su última velocidad). Un **Stop Montage** antes del final desvanece el movimiento junto con su peso.

## Motion Warping (como el de Unreal)
- En **Motion Warping**, **Add Warp Window**: el nombre del objetivo (`Target`), el inicio y el fin de la ventana (segundos del clip) y qué corregir: **Place**, **Facing**, **Ignore Height**.
- Al reproducir, **Add or Update Warp Target** (Target, Warp Target Name, Location, Rotation) dice dónde debe terminar la ventana. Dentro de la ventana el recorrido del clip se curva poco a poco, según el propio avance del clip, y el actor termina **exactamente** en el lugar y la orientación del objetivo.
- Usos: un ataque que alcanza al enemigo esté donde esté, saltar un muro de cualquier altura (desmarca Ignore Height), la mano en la manija de una puerta, sentarse en una silla.
- **Remove Warp Target** olvida el objetivo: la ventana reproduce el clip tal como está.

```blueprint
Event Key F -> Add or Update Warp Target ( Target = self, Warp Target Name = "Target", Location = Get Actor Location( Get Player Character ), Rotation = Get Actor Rotation( Get Player Character ) )
            -> Play Montage ( Target = self, Play Rate = 1 )
```
El montage (con Root Motion y una ventana sobre el objetivo `Target`) va en los Detalles del Play Montage. Costo: toda la animación de dos personajes con root motion y un warp se queda en 0,012 ms por cuadro (la prueba `warp`).
