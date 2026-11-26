id: locomotion-styles
kind: recipe
title: Estilos de locomoção: ferido, cansado, bêbado, carregando | Locomotion styles: injured, tired, drunk, carrying | Estilos de locomoción: herido, cansado, borracho, cargando
keys: estilo de locomocao, locomotion style, andar ferido, injured walk, mancar, limp, cansado, tired, bebado, drunk, carregando, carrying, carregar caixa, carry a box, jeito de andar, walk style, arquetipo, archetype, motion matching estilo, motion matching style, set locomotion style, get locomotion style
--- pt
Cada personagem pode andar de um **jeito** diferente, como os arquétipos do GTA 6, sem trocar o Anim Graph:
- **Set Locomotion Style** (Target, Style): **Normal**, **Injured** (mancando), **Tired** (curvado, cabeça baixa), **Drunk** (balançando), **Carrying** (braços à frente segurando algo). **Get Locomotion Style** diz o atual.
- **Com clipes próprios**: ponha no estado de **Motion Matching** animações com a palavra do estilo no nome (por exemplo `MM_Walk_Injured`, `MM_Run_Tired`, `MM_Walk_Carry`). Com o estilo ligado, a busca só usa essas; sem ele, só as normais.
- **Sem clipes**: uma camada leve dobra o corpo (a pelve, a coluna, a cabeça, os braços, o joelho).
- **Sem salto**: a troca entra suave (cerca de 0,6 s), em qualquer momento do passo.
- **Ferido sozinho**: se o personagem tem Ragdoll Ativo e levou um tiro na perna (**Apply Wound Reaction**), ele manca pelo **Get Limp**, sem precisar chamar nada.
- **Custo**: 0,0006 ms por personagem.

```blueprint
Event Key 1 -> Set Locomotion Style ( Target = self, Style = "Carrying" )
Event Key 2 -> Set Locomotion Style ( Target = self, Style = "Normal" )
```
--- en
Each character can walk a different **way**, like GTA 6's archetypes, without changing its Anim Graph:
- **Set Locomotion Style** (Target, Style): **Normal**, **Injured** (limping), **Tired** (bowed, head down), **Drunk** (swaying), **Carrying** (arms ahead holding something). **Get Locomotion Style** tells the current one.
- **With its own clips**: put animations with the style's word in their names in the **Motion Matching** state (for example `MM_Walk_Injured`, `MM_Run_Tired`, `MM_Walk_Carry`). With the style on, the search uses only those; without it, only the normal ones.
- **Without clips**: a light layer bends the body (the pelvis, the spine, the head, the arms, the knee).
- **No pop**: the switch eases in (about 0.6 s), at any moment of the step.
- **Injured by itself**: if the character has an Active Ragdoll and took a shot in the leg (**Apply Wound Reaction**), it limps by its **Get Limp**, with nothing to call.
- **Cost**: 0.0006 ms per character.

```blueprint
Event Key 1 -> Set Locomotion Style ( Target = self, Style = "Carrying" )
Event Key 2 -> Set Locomotion Style ( Target = self, Style = "Normal" )
```
--- es
Cada personaje puede caminar de una **forma** distinta, como los arquetipos de GTA 6, sin cambiar su Anim Graph:
- **Set Locomotion Style** (Target, Style): **Normal**, **Injured** (cojeando), **Tired** (encorvado, cabeza baja), **Drunk** (tambaleándose), **Carrying** (brazos adelante sosteniendo algo). **Get Locomotion Style** indica el actual.
- **Con clips propios**: pon animaciones con la palabra del estilo en su nombre en el estado de **Motion Matching** (por ejemplo `MM_Walk_Injured`, `MM_Run_Tired`, `MM_Walk_Carry`). Con el estilo activo, la búsqueda usa solo esos; sin él, solo los normales.
- **Sin clips**: una capa ligera dobla el cuerpo (la pelvis, la columna, la cabeza, los brazos, la rodilla).
- **Sin saltos**: el cambio se suaviza (unos 0.6 s), en cualquier momento del paso.
- **Herido por sí solo**: si el personaje tiene un Active Ragdoll y recibió un disparo en la pierna (**Apply Wound Reaction**), cojea según su **Get Limp**, sin nada que llamar.
- **Costo**: 0.0006 ms por personaje.

```blueprint
Event Key 1 -> Set Locomotion Style ( Target = self, Style = "Carrying" )
Event Key 2 -> Set Locomotion Style ( Target = self, Style = "Normal" )
```
