id: paired-montage
kind: recipe
title: Montage pareado: dois personagens na mesma animação, em sincronia | Paired montage: two characters in one animation, in step | Montage emparejado: dos personajes en una animación, al compás
keys: montage pareado, paired montage, animacao sincronizada, synced animation, sincronia, in sync, dois personagens, two characters, finalizacao, takedown, finisher, execucao, execution, agarrar, grab, aperto de mao, handshake, abraco, hug, cena de luta, fight scene, interacao entre personagens, character interaction, contextual animation, play paired montage, stop paired montage, partner montage, parceiro, partner
--- pt
```diagram steps
Primário (o centro) | Play Paired Montage | Parceiro desliza ao lugar (Align Time) | Os dois no mesmo quadro até o fim
```
## Montar o par
- Faça dois montages, um para cada personagem (por exemplo `AM_Takedown_Attacker` e `AM_Takedown_Victim`), animados juntos no programa de animação.
- No montage do **primário** (quem é o centro da cena), em **Montage Pareado**: **Montage do Parceiro** (o do outro), **Posição do Parceiro** (cm para a frente, para a direita e para cima do primário), **Giro do Parceiro** (180: de frente um para o outro) e **Tempo de Alinhamento** (os segundos em que o parceiro desliza até o lugar).
- Os dois podem ter **Root Motion**: o primário anda pelo clipe dele e o parceiro acompanha preso a ele, com o próprio caminho do clipe por cima.

## Tocar
- **Play Paired Montage** (Primary, Partner, Montage, Play Rate): os dois começam no **mesmo quadro**, na mesma velocidade, e ficam travados juntos até o fim. O parceiro para de andar (o input dele espera).
- **Stop Paired Montage** (Target, Blend Out) termina o par dos dois, seja o Target o primário ou o parceiro. Quando o montage do primário acaba, o do parceiro acaba junto.
- Para um ataque, use um **Anim Notify** no montage do atacante (por exemplo `Hit`) e aplique o dano no parceiro nesse evento.

```blueprint
Event Key E -> Play Paired Montage ( Primary = self, Partner = Get Player Character, Montage = "AM_Takedown_Attacker", Play Rate = 1 )
```
Custo: a animação inteira do par fica em 0,012 ms por quadro (a medida do teste `warp`: o tempo do parceiro bate com o do primário em 0,0000 s e ele termina a 0,1 cm do lugar).

## Escolher a variante e checar antes
- Faça um **conjunto** de montages pareados com o mesmo começo de nome (por exemplo `AM_Takedown_Front`, `AM_Takedown_Back`, `AM_Takedown_Left`), cada um com a sua Posição e Giro do Parceiro.
- **Find Paired Montage** (Primary, Partner, Prefix, Max Distance, Max Angle) devolve o do conjunto cujo lugar do parceiro mais combina com onde ele está e para onde olha agora: pela frente, pelas costas ou pelo lado, um nó só.
- **Can Play Paired Montage** confere a distância, o ângulo e se não tem parede entre os dois: use antes de tocar.
--- en
```diagram steps
Primary (the center) | Play Paired Montage | Partner slides into place (Align Time) | Both on the same frame to the end
```
## Set up the pair
- Make two montages, one for each character (for example `AM_Takedown_Attacker` and `AM_Takedown_Victim`), animated together in the animation tool.
- In the **primary's** montage (the one at the center of the scene), under **Paired Montage**: **Partner Montage** (the other's), **Partner Offset** (cm ahead, to the right and up from the primary), **Partner Yaw** (180: facing each other) and **Align Time** (the seconds the partner slides into its place).
- Both may have **Root Motion**: the primary walks by its clip and the partner follows locked to it, its own clip's path on top.

## Play
- **Play Paired Montage** (Primary, Partner, Montage, Play Rate): both start on the **same frame**, at the same rate, and stay locked together to the end. The partner stops walking (its input waits).
- **Stop Paired Montage** (Target, Blend Out) ends the pair for both, whether the Target is the primary or the partner. When the primary's montage ends, the partner's ends with it.
- For an attack, put an **Anim Notify** in the attacker's montage (for example `Hit`) and apply the damage to the partner on that event.

```blueprint
Event Key E -> Play Paired Montage ( Primary = self, Partner = Get Player Character, Montage = "AM_Takedown_Attacker", Play Rate = 1 )
```
Cost: the pair's whole animation stays at 0.012 ms a frame (the `warp` test's measure: the partner's time matches the primary's within 0.0000 s and it ends 0.1 cm from its place).

## Pick the variant and check first
- Make a **set** of paired montages with the same name start (for example `AM_Takedown_Front`, `AM_Takedown_Back`, `AM_Takedown_Left`), each with its own Partner Offset and Yaw.
- **Find Paired Montage** (Primary, Partner, Prefix, Max Distance, Max Angle) returns the one of the set whose partner place best fits where the partner stands and faces now: from the front, the back or the side, one node.
- **Can Play Paired Montage** checks the distance, the angle and that no wall is between the two: use it before playing.
--- es
```diagram steps
Primario (el centro) | Play Paired Montage | El compañero se desliza a su lugar (Align Time) | Ambos en el mismo cuadro hasta el final
```
## Configura la pareja
- Crea dos montages, uno para cada personaje (por ejemplo `AM_Takedown_Attacker` y `AM_Takedown_Victim`), animados juntos en la herramienta de animación.
- En el montage del **primario** (el del centro de la escena), en **Paired Montage**: **Partner Montage** (el del otro), **Partner Offset** (cm hacia adelante, a la derecha y arriba del primario), **Partner Yaw** (180: cara a cara) y **Align Time** (los segundos que el compañero tarda en deslizarse a su lugar).
- Ambos pueden tener **Root Motion**: el primario camina según su clip y el compañero lo sigue anclado a él, con el recorrido de su propio clip encima.

## Reproduce
- **Play Paired Montage** (Primary, Partner, Montage, Play Rate): ambos empiezan en el **mismo cuadro**, a la misma velocidad, y siguen anclados juntos hasta el final. El compañero deja de caminar (su input espera).
- **Stop Paired Montage** (Target, Blend Out) termina la pareja para ambos, sea el Target el primario o el compañero. Cuando el montage del primario termina, el del compañero termina con él.
- Para un ataque, pon un **Anim Notify** en el montage del atacante (por ejemplo `Hit`) y aplica el daño al compañero en ese evento.

```blueprint
Event Key E -> Play Paired Montage ( Primary = self, Partner = Get Player Character, Montage = "AM_Takedown_Attacker", Play Rate = 1 )
```
Costo: toda la animación de la pareja se mantiene en 0.012 ms por cuadro (la medida de la prueba `warp`: el tiempo del compañero coincide con el del primario con 0.0000 s de diferencia y termina a 0.1 cm de su lugar).

## Elige la variante y comprueba antes
- Crea un **conjunto** de montages emparejados con el mismo comienzo de nombre (por ejemplo `AM_Takedown_Front`, `AM_Takedown_Back`, `AM_Takedown_Left`), cada uno con su propio Partner Offset y Yaw.
- **Find Paired Montage** (Primary, Partner, Prefix, Max Distance, Max Angle) devuelve el del conjunto cuyo lugar para el compañero mejor se ajusta a donde este está y mira ahora: de frente, por detrás o de lado, con un solo nodo.
- **Can Play Paired Montage** comprueba la distancia, el ángulo y que no haya una pared entre los dos: úsalo antes de reproducir.
