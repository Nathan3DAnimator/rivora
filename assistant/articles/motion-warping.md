id: motion-warping
kind: recipe
title: Root Motion e Motion Warping: alinhar o personagem a um alvo | Root Motion and Motion Warping: align the character to a target
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
