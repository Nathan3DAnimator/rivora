id: locomotion-styles
kind: recipe
title: Estilos de locomoção: ferido, cansado, bêbado, carregando | Locomotion styles: injured, tired, drunk, carrying
keys: estilo de locomocao, locomotion style, andar ferido, injured walk, mancar, limp, cansado, tired, bebado, drunk, carregando, carrying, carregar caixa, carry a box, jeito de andar, walk style, arquetipo, archetype, motion matching estilo, motion matching style, set locomotion style, get locomotion style
--- pt
Cada personagem pode andar de um **jeito** diferente, como os arquétipos do GTA 6, sem trocar o Anim Blueprint:
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
Each character can walk a different **way**, like GTA 6's archetypes, without changing its Anim Blueprint:
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
