id: ragdoll-realistic
kind: recipe
title: Ragdoll realista: limites anatômicos, tônus, proteção e levantar | Realistic ragdoll: anatomical limits, tone, bracing and getting up | Ragdoll realista
keys: ragdoll realista, realistic ragdoll, limites das juntas, joint limits, limite anatomico, anatomical limits, joelho, knee, cotovelo, elbow, dobradica, hinge, coluna, spine, pescoco, neck, ombro, shoulder, quadril, hip, torcao, twist, tonus, tone, boneco de pano, rag doll, mole, floppy, massa pelo volume, mass by volume, densidade, density, autocolisao, self collision, pernas atravessando, legs crossing, se proteger, brace, maos no chao, hands on the ground, cair de bruços, face down, de costas, on its back, montagem de levantar, get up montage, so o braco, arm alone, peso da fisica por osso, physics blend weight below, set all bodies below physics blend weight, is lying face down
--- pt
O ragdoll do **Physics Asset** cai como um corpo, não como um trapo: cada junta tem **limites anatômicos** medidos da pose de bind (iguais em qualquer pose em que ele comece), a **massa** de cada corpo vem do volume da forma, as partes que não são vizinhas **colidem entre si** (as pernas não se atravessam) e o **amortecimento** e o **tônus** das juntas o fazem assentar em vez de tremer.
- **Tipo de Junta** (editor do Physics Asset > uma junta): Dobradiça (joelho só dobra para trás, cotovelo só para a frente, quase nada para os lados), Coluna, Pescoço, Ombro, Quadril, Pulso / Tornozelo, Cauda, Esfera. Escolher um põe os limites; **Dobra**, **Lados** e **Torção** (mín e máx, assimétricos) ajustam; **Padrões Anatômicos** volta ao tipo.
- **Configurações do Ragdoll**: **Densidade** e **Massas pelo Volume** (um corpo humano: 1,0 g/cm³), **Tônus do Ragdoll** (o quanto da pose ele mantém ao cair; solta ao deitar).
- **Ragdoll Ativo**: ao tropeçar e cair os **braços se esticam para a frente e para baixo** e as mãos chegam ao chão antes da cabeça. Caído, ele sabe se está **de bruços** ou **de costas** (**Is Lying Face Down**); com as montagens **Levantar de Bruços** / **Levantar de Costas**, a primeira pose da montagem é casada com o corpo (lugar e direção) e ele se mistura nela.
- **Por osso**: **Set All Bodies Below Simulate Physics** solta só um braço; **Set All Bodies Below Physics Blend Weight** devolve partes do corpo para a animação com um tempo de mistura.
- **Custo**: ~0,11 ms de CPU por ragdoll de 19 corpos caindo; ~0,05 ms por Ragdoll Ativo em pé; 50 Ragdolls Ativos: 2,5 ms de CPU no quadro (385 fps).

```diagram steps
Physics Asset | Generate Bodies (tipos de junta) | Play | cai como um corpo | Ragdoll Ativo: se protege e levanta
```
```blueprint
Event Begin Play -> Set Active Ragdoll ( Target = self, Enabled = true )
Event Key G -> Set All Bodies Below Physics Blend Weight ( Target = self, Bone Name = "upperarm_l", Weight = 0, Blend Time = 0.3 )
```
--- en
The **Physics Asset**'s ragdoll falls as a body, not a rag: each joint has **anatomical limits** measured from the bind pose (the same whatever pose it starts in), each body's **mass** comes from its shape's volume, the parts that are not neighbors **collide with each other** (the legs never pass through each other), and the joints' **damping** and **tone** let it settle instead of trembling.
- **Joint Kind** (Physics Asset editor > a joint): Hinge (a knee folds backward only, an elbow forward only, barely to the sides), Spine, Neck, Shoulder, Hip, Wrist / Ankle, Tail, Ball. Choosing one sets the limits; **Bend**, **Sides** and **Twist** (min and max, asymmetric) tune them; **Anatomical Defaults** goes back to the kind's.
- **Ragdoll Settings**: **Density** and **Masses from Volume** (a human body: 1.0 g/cm³), **Ragdoll Tone** (how much of its pose it keeps as it falls; it lets go once it lies).
- **Active Ragdoll**: stumbling and falling, **its arms reach forward and down** and the hands meet the ground before the head. Down, it knows whether it lies **face down** or **on its back** (**Is Lying Face Down**); with the **Get Up Face Down** / **Get Up On Its Back** montages, the montage's first pose is matched to the body (place and heading) and it blends into it.
- **By bone**: **Set All Bodies Below Simulate Physics** lets one arm go; **Set All Bodies Below Physics Blend Weight** hands parts of the body back to the animation over a blend time.
- **Cost**: ~0.11 ms of CPU per falling 19-body ragdoll; ~0.05 ms per standing Active Ragdoll; 50 Active Ragdolls: 2.5 ms of CPU a frame (385 fps).

```diagram steps
Physics Asset | Generate Bodies (joint kinds) | Play | falls as a body | Active Ragdoll: braces and gets up
```
```blueprint
Event Begin Play -> Set Active Ragdoll ( Target = self, Enabled = true )
Event Key G -> Set All Bodies Below Physics Blend Weight ( Target = self, Bone Name = "upperarm_l", Weight = 0, Blend Time = 0.3 )
```
--- es
El ragdoll del **Physics Asset** cae como un cuerpo, no como un trapo: cada articulación tiene **límites anatómicos** medidos a partir de la bind pose (los mismos sea cual sea la pose con que empiece), la **masa** de cada cuerpo sale del volumen de su forma, las partes que no son vecinas **colisionan entre sí** (las piernas nunca se atraviesan), y el **amortiguamiento** y el **tono** de las articulaciones lo dejan asentarse en lugar de temblar.
- **Joint Kind** (editor del Physics Asset > una articulación): Hinge (una rodilla solo se dobla hacia atrás, un codo solo hacia adelante, apenas hacia los lados), Spine, Neck, Shoulder, Hip, Wrist / Ankle, Tail, Ball. Elegir uno fija los límites; **Bend**, **Sides** y **Twist** (mín. y máx., asimétricos) los ajustan; **Anatomical Defaults** vuelve a los del tipo.
- **Ragdoll Settings**: **Density** y **Masses from Volume** (un cuerpo humano: 1,0 g/cm³), **Ragdoll Tone** (cuánto conserva de su pose al caer; la suelta cuando ya está en el suelo).
- **Active Ragdoll**: al tropezar y caer, **sus brazos se estiran hacia adelante y hacia abajo** y las manos llegan al suelo antes que la cabeza. En el suelo, sabe si está **boca abajo** o **de espaldas** (**Is Lying Face Down**); con los montages **Get Up Face Down** / **Get Up On Its Back**, la primera pose del montage se ajusta al cuerpo (lugar y orientación) y se mezcla con ella.
- **Por hueso**: **Set All Bodies Below Simulate Physics** suelta un brazo; **Set All Bodies Below Physics Blend Weight** devuelve partes del cuerpo a la animación durante un tiempo de mezcla.
- **Costo**: ~0,11 ms de CPU por ragdoll de 19 cuerpos en caída; ~0,05 ms por Active Ragdoll de pie; 50 Active Ragdolls: 2,5 ms de CPU por cuadro (385 fps).

```diagram steps
Physics Asset | Generate Bodies (tipos de articulación) | Jugar | cae como un cuerpo | Active Ragdoll: se protege y se levanta
```
```blueprint
Event Begin Play -> Set Active Ragdoll ( Target = self, Enabled = true )
Event Key G -> Set All Bodies Below Physics Blend Weight ( Target = self, Bone Name = "upperarm_l", Weight = 0, Blend Time = 0.3 )
```
