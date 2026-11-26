id: anim-warping
kind: howto
title: Orientation e Stride Warping: pernas na direção do movimento, passos na velocidade | Orientation and Stride Warping: legs toward the move, steps at the speed | Orientation y Stride Warping: piernas hacia el movimiento, pasos a la velocidad
keys: orientation warping, stride warping, animation warping, andar de lado, strafe, strafing, pe deslizando, foot sliding, pes escorregando, feet sliding, passada, stride, velocidade da animacao, animation speed, pernas viram, legs turn, peito de frente, chest forward, locomocao, locomotion, anim graph
--- pt
```diagram steps
Anim Graph | Orientation Warping e Stride Warping antes do Output Pose | Pernas para onde anda, peito de frente | Passos esticados pela velocidade
```
## Orientation Warping
- No **Anim Graph**, ponha **Orientation Warping** (Skeletal Controls) antes do **Output Pose**. Quando o Character anda para um lado diferente de onde olha (com **Use Controller Rotation Yaw**, mirando), ele gira a **pelve** para a direção do movimento e devolve o giro pela **coluna**: as pernas andam para lá e o peito continua de frente, usando só a caminhada para a frente.
- **Ângulo Máximo** limita o giro; **Quatro Direções** gira só o que passa da direção do clipe mais perto (frente, trás, esquerda, direita), para quem tem os quatro clipes; **Velocidade do Giro** suaviza mudanças.

## Stride Warping
- **Stride Warping** estica ou encurta a **passada** pela velocidade real do Character contra a da animação (lida do pé apoiado sozinha): andando mais rápido ou mais devagar que o clipe, **os pés não deslizam**.
- **Escala da Passada** (mínima e máxima) limita; **Descida da Pelve** abaixa o quadril nas passadas longas.
- Medido: andando a 45° da frente, o quadril gira 46,5° e o peito fica a 0,2°; a velocidade lida da animação (143 cm/s) bate com a do Character (150).
--- en
```diagram steps
Anim Graph | Orientation Warping and Stride Warping before the Output Pose | Legs toward the move, chest forward | Steps stretched by the speed
```
## Orientation Warping
- In the **Anim Graph**, put **Orientation Warping** (Skeletal Controls) before the **Output Pose**. When the Character moves another way than it faces (with **Use Controller Rotation Yaw**, aiming), it turns the **pelvis** toward the move and gives the turn back along the **spine**: the legs head that way and the chest keeps facing ahead, with only the forward clip.
- **Max Angle** limits the turn; **Four Directions** turns only the rest past the nearest clip's direction (forward, back, left, right), for those with the four clips; **Turn Speed** eases changes.

## Stride Warping
- **Stride Warping** stretches or shortens the **stride** by the Character's real speed against the animation's (read from its planted foot by itself): faster or slower than the clip, **the feet don't slide**.
- **Stride Scale** (min and max) limits it; **Pelvis Drop** lowers the hips on long strides.
- Measured: moving 45 degrees off its facing, the hips turn 46.5 and the chest stays at 0.2; the animation's speed read (143 cm/s) matches the Character's (150).
--- es
```diagram steps
Anim Graph | Orientation Warping y Stride Warping antes del Output Pose | Piernas hacia el movimiento, pecho al frente | Pasos estirados según la velocidad
```
## Orientation Warping
- En el **Anim Graph**, pon **Orientation Warping** (Skeletal Controls) antes del **Output Pose**. Cuando el Character se mueve en una dirección distinta a la que mira (con **Use Controller Rotation Yaw**, al apuntar), gira la **pelvis** hacia el movimiento y devuelve el giro a lo largo de la **columna**: las piernas van en esa dirección y el pecho sigue mirando al frente, con solo el clip hacia adelante.
- **Max Angle** limita el giro; **Four Directions** gira solo el resto más allá de la dirección del clip más cercano (adelante, atrás, izquierda, derecha), para quien tiene los cuatro clips; **Turn Speed** suaviza los cambios.

## Stride Warping
- **Stride Warping** estira o acorta la **zancada** según la velocidad real del Character frente a la de la animación (la lee por sí solo a partir de su pie apoyado): más rápido o más lento que el clip, **los pies no resbalan**.
- **Stride Scale** (mínimo y máximo) la limita; **Pelvis Drop** baja las caderas en zancadas largas.
- Medido: al moverse 45 grados fuera de su orientación, las caderas giran 46,5 y el pecho queda en 0,2; la velocidad leída de la animación (143 cm/s) coincide con la del Character (150).
