id: horror-kit
kind: recipe
title: Kit de terror: tensão, filtros de choque, exposição escura, luzes que piscam | Horror kit: tension, shock filters, dark exposure, flickering lights | Kit de terror
keys: terror, horror, medo, fear, susto, jump scare, scare, tensao, tension, diretor de tensao, tension director, set tension, add tension, get tension, filtro, filter, alucinacao, hallucination, preto e branco, black and white, estourado, blown out, flash branco, white flash, estatica, tv static, chuvisco, vhs, fita, aberracao cromatica, chromatic aberration, distorcao, distortion, letterbox, barras pretas, black bars, cinemascope, dip to black, fade to black, mergulho no preto, dip to white, dolly zoom, vertigo, chicote, whip pan, dutch, holandes, camera inclinada, exposicao de terror, horror exposure, escuro, dark, pretos, blacks, toe, piso de ev, ev floor, luz piscando, flicker, light flicker, vela, candle, lampada defeituosa, faulty bulb, estrobo, strobe, relampago, lightning, rim light, luz de recorte, silhueta, silhouette, contraluz, backlight, canais de luz, lighting channels, luz de heroi, hero light, olhos brilhando, glowing eyes, eye glow, lanterna, flashlight, poeira, dust, feixe, beam, sound graph, grafo de som
--- pt
O **kit de terror** junta o que faz um curta de terror funcionar: o escuro de verdade, uma luz motivada, silhuetas, olhos brilhando no preto, choques de imagem e a tensão crescendo.
1. **Diretor de Tensão**: um valor de 0 a 1 que um Grafo guia (**Set Tension**, **Add Tension**, **Get Tension**). Ele sobe e desce suave e comanda, por curvas, o grão, a vinheta, a saturação, o letterbox, o campo de visão, uma **pulsação** de coração, o **piscar das luzes** e o som (o volume e o abafado de uma Sound Class, e uma entrada de Sound Graph). As curvas ficam no Post Process Volume > **Terror** > Curvas de Tensão (o valor de cada canal na tensão 0, 1/3, 2/3 e 1).
2. **Filtros de choque**: **Trigger Horror Filter** (um evento: entra, segura, sai) e **Set Horror Filter** (fica ligado): Alucinação (ruído colorido grosso, vermelho, posterizado, pretos esmagados), P&B Estourado, Flash Branco, Estática de TV, VHS, Aberração Cromática e Distorção. Todos juntos custam ~0,2 ms em 1440p. No Sequencer: + Trilha > **Filtro de Terror** (cada chave um filtro e a intensidade).
3. **Exposição de terror**: Post Process Volume > Terror > **Exposição de Terror**. A exposição automática mede o que está iluminado, não a média; o **Piso de EV** impede que ela clareie um quarto escuro; o **Pé da Curva** empurra as sombras para o preto sem levantar nada.
4. **Luzes**: Details > **Cintilação da Luz**: Vela, Lâmpada Defeituosa, Estrobo, Relâmpago, Pulsação, com um som sincronizado (o trovão com atraso). **Luz de Recorte** segue o personagem por trás, vista da câmera. **Canais de Iluminação** (Details da luz e da malha): a luz só ilumina quem divide um canal com ela. **Poeira no Feixe**: a lanterna mostra poeira na neblina volumétrica.
5. **Olhos que brilham**: um material com Shading Model **Eye** e **Brilho dos Olhos** devolve a luz da lanterna (o exemplo `Examples/M_EyeGlow`).
6. **Câmera**: **Set Letterbox** (barras até 2,39:1), **Dip to Black / Dip to White**, **Play Scare Rig** (Dolly Zoom, Chicote, Ângulo Holandês).

```diagram steps
Post Process Volume: Terror > Exposição de Terror | Lâmpadas: Cintilação Vela / Lâmpada Defeituosa | Monstro perto: Set Tension 1 | Pegou: Flash Branco + Alucinação + Dip to Black | Escapou: Dip to White
```
```blueprint
Event BeginPlay
  -> Set Tension ( Tension = 0.8, Blend Time = 3 )
  -> Trigger Horror Filter ( Filter = "White Flash", Intensity = 1, Fade In = 0, Hold = 0.05, Fade Out = 0.15 )
  -> Play Scare Rig ( Rig = "Dutch Angle", Intensity = 1, Duration = 2.5 )
  -> Dip to Black ( Fade In = 0.25, Hold = 0.8, Fade Out = 1.2 )
```
--- en
The **horror kit** gathers what makes a horror short work: real darkness, one motivated light, silhouettes, eyes shining in the black, image shocks and the tension building.
1. **Tension Director**: a value from 0 to 1 a Graph drives (**Set Tension**, **Add Tension**, **Get Tension**). It rises and falls smoothly and drives, by curves, the grain, the vignette, the saturation, the letterbox, the field of view, a heartbeat **pulse**, the **lights' flicker** and the sound (a Sound Class's volume and muffling, and a Sound Graph input). The curves live in the Post Process Volume > **Horror** > Tension Curves (each channel's value at tension 0, 1/3, 2/3 and 1).
2. **Shock filters**: **Trigger Horror Filter** (an event: in, hold, out) and **Set Horror Filter** (held on): Hallucination (coarse colored noise, red, posterized, crushed blacks), Black & White Blown Out, White Flash, TV Static, VHS, Chromatic Aberration and Distortion. All of them together cost ~0.2 ms at 1440p. In the Sequencer: + Track > **Horror Filter** (each key a filter and its intensity).
3. **Horror exposure**: Post Process Volume > Horror > **Horror Exposure**. The auto exposure meters what is lit, not the mean; the **EV Floor** keeps it from brightening a dark room; the **Toe** pushes the shadows to black without lifting anything.
4. **Lights**: Details > **Light Flicker**: Candle, Faulty Bulb, Strobe, Lightning, Pulse, with a sound in sync (the thunder after a delay). **Rim Light** follows a character from behind as the camera sees it. **Lighting Channels** (the light's and the mesh's Details): the light lights only what shares a channel with it. **Beam Dust**: the flashlight shows dust in the volumetric fog.
5. **Eyes that shine**: a material with the **Eye** Shading Model and **Eye Glow** sends the flashlight's light back (the example `Examples/M_EyeGlow`).
6. **Camera**: **Set Letterbox** (bars down to 2.39:1), **Dip to Black / Dip to White**, **Play Scare Rig** (Dolly Zoom, Whip Pan, Dutch Angle).

```diagram steps
Post Process Volume: Horror > Horror Exposure | Lamps: Candle / Faulty Bulb flicker | Monster near: Set Tension 1 | Caught: White Flash + Hallucination + Dip to Black | Escaped: Dip to White
```
```blueprint
Event BeginPlay
  -> Set Tension ( Tension = 0.8, Blend Time = 3 )
  -> Trigger Horror Filter ( Filter = "White Flash", Intensity = 1, Fade In = 0, Hold = 0.05, Fade Out = 0.15 )
  -> Play Scare Rig ( Rig = "Dutch Angle", Intensity = 1, Duration = 2.5 )
  -> Dip to Black ( Fade In = 0.25, Hold = 0.8, Fade Out = 1.2 )
```
--- es
El **kit de terror** reúne lo que hace funcionar un corto de terror: oscuridad real, una luz motivada, siluetas, ojos que brillan en la negrura, sobresaltos de imagen y la tensión que va creciendo.
1. **Tension Director**: un valor de 0 a 1 que controla un Grafo (**Set Tension**, **Add Tension**, **Get Tension**). Sube y baja con suavidad y controla, mediante curvas, el grano, la viñeta, la saturación, el letterbox, el campo de visión, un **pulso** de latido, el **parpadeo de las luces** y el sonido (el volumen y la atenuación de una Sound Class, y una entrada de un Sound Graph). Las curvas viven en Post Process Volume > **Horror** > Tension Curves (el valor de cada canal con tensión 0, 1/3, 2/3 y 1).
2. **Filtros de impacto**: **Trigger Horror Filter** (un evento: entrada, mantener, salida) y **Set Horror Filter** (mantenido): Hallucination (ruido de color grueso, rojo, posterizado, negros aplastados), Black & White Blown Out, White Flash, TV Static, VHS, Chromatic Aberration y Distortion. Todos juntos cuestan ~0,2 ms a 1440p. En el Sequencer: + Pista > **Horror Filter** (cada clave es un filtro y su intensidad).
3. **Exposición de terror**: Post Process Volume > Horror > **Horror Exposure**. La exposición automática mide lo que está iluminado, no la media; el **EV Floor** evita que aclare un cuarto oscuro; el **Toe** empuja las sombras hacia el negro sin levantar nada.
4. **Luces**: Detalles > **Light Flicker**: Candle, Faulty Bulb, Strobe, Lightning, Pulse, con un sonido sincronizado (el trueno después de un retraso). **Rim Light** sigue a un personaje por detrás tal como lo ve la cámara. **Lighting Channels** (en Detalles de la luz y de la malla): la luz ilumina solo lo que comparte un canal con ella. **Beam Dust**: la linterna muestra polvo en la niebla volumétrica.
5. **Ojos que brillan**: un material con el Shading Model **Eye** y **Eye Glow** devuelve la luz de la linterna (el ejemplo `Examples/M_EyeGlow`).
6. **Cámara**: **Set Letterbox** (barras hasta 2,39:1), **Dip to Black / Dip to White**, **Play Scare Rig** (Dolly Zoom, Whip Pan, Dutch Angle).

```diagram steps
Post Process Volume: Horror > Horror Exposure | Lámparas: Candle / Faulty Bulb parpadean | Monstruo cerca: Set Tension 1 | Atrapado: White Flash + Hallucination + Dip to Black | Escapó: Dip to White
```
```blueprint
Event BeginPlay
  -> Set Tension ( Tension = 0.8, Blend Time = 3 )
  -> Trigger Horror Filter ( Filter = "White Flash", Intensity = 1, Fade In = 0, Hold = 0.05, Fade Out = 0.15 )
  -> Play Scare Rig ( Rig = "Dutch Angle", Intensity = 1, Duration = 2.5 )
  -> Dip to Black ( Fade In = 0.25, Hold = 0.8, Fade Out = 1.2 )
```
