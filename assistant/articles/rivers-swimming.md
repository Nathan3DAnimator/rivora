id: rivers-swimming
kind: concept
title: Rios, flutuação, nado e debaixo d'água | Rivers, buoyancy, swimming and underwater | Ríos, flotación, nado y bajo el agua
keys: character swim, character should swim, swim in the lake, swim in water, personagem nadar, nadar no lago, rio, river, water body river, correnteza, current, spline de rio, river spline, flutuar, float, boiar, buoyancy, buoyancy component, caixa flutuando, objeto flutuando, nadar, swim, swimming, nado, personagem nadando, character swimming, debaixo d'agua, debaixo da agua, underwater, embaixo d'agua, linha d'agua, waterline, mergulhar, dive, REALENGINE_NO_UNDERWATER
--- pt
```diagram steps
Water Body River: uma spline com largura, profundidade e velocidade por ponto
A água corre ao longo dela (as ondas descem o rio)
Buoyancy: um corpo com física flutua e a correnteza o leva
O Character nada quando a água passa do peito; a câmera embaixo d'água muda a imagem
```
**Rio:** **Adicionar > Efeitos Visuais > Water Body River** (como o Water Body River da Unreal). Em Details > **River**, cada ponto tem lugar, **W** (largura), **D** (profundidade) e **V** (velocidade); **Add Point** estende o rio. A cor, a transparência e a espuma vêm do **Water Body** do mesmo ator. A água desenha as ondas correndo para baixo e fica mais lenta nas margens.

**Flutuar:** num ator com **Simulate Physics**, **+ Adicionar > Buoyancy**. Ele flutua em qualquer água (lago, rio, oceano), endireita nas ondas e a correnteza o carrega. **Buoyancy** (1 = madeira; 2 = boia alta; abaixo de 0,5 afunda), **Water Drag** (quanto a água puxa) e **Pontoons** (1 ou 5 pontos de apoio).

**Nadar:** o **Character** nada sozinho quando a água passa do peito: sem gravidade, cabeça fora d'água, mais devagar, e a correnteza leva. **Jump** sobe; na água rasa ele anda mais devagar. **Swim Speed** (0 = 55% da Max Walk Speed).

**Debaixo d'água:** com a câmera dentro da água, a imagem fica azul-esverdeada (o vermelho some primeiro), mais escura quanto mais fundo e com uma leve ondulação. Com a lente na superfície, a linha d'água corta a tela. Para comparar sem: `REALENGINE_NO_UNDERWATER=1`.

Cenas de teste: `river` (com Play) e `underwater`.
--- en
```diagram steps
Water Body River: a spline with width, depth and speed per point
The water runs along it (the waves go downstream)
Buoyancy: a physics body floats and the current takes it
The Character swims when the water passes its chest; the camera under water changes the picture
```
**River:** **Add > Visual Effects > Water Body River** (like Unreal's Water Body River). In Details > **River**, each point has its place, **W** (width), **D** (depth) and **V** (speed); **Add Point** extends the river. The color, clarity and foam come from the same actor's **Water Body**. The water draws its waves running downstream and runs slower at the banks.

**Floating:** on an actor with **Simulate Physics**, **+ Add > Buoyancy**. It floats in any water (lake, river, ocean), rights itself on the waves and the current carries it. **Buoyancy** (1 = wood; 2 = a high buoy; under 0.5 it sinks), **Water Drag** (how much the water pulls) and **Pontoons** (1 or 5 support points).

**Swimming:** a **Character** swims by itself when the water passes its chest: no gravity, its head above the water, slower, and the current carries it. **Jump** swims up; in shallow water it walks slower. **Swim Speed** (0 = 55% of Max Walk Speed).

**Under water:** with the camera in the water, the picture turns blue-green (red goes first), darker the deeper, with a slight waver. With the lens at the surface, the waterline cuts the screen. To compare without it: `REALENGINE_NO_UNDERWATER=1`.

Test scenes: `river` (with Play) and `underwater`.
--- es
```diagram steps
Water Body River: una spline con ancho, profundidad y velocidad por punto
El agua corre a lo largo de ella (las olas van río abajo)
Buoyancy: un cuerpo con física flota y la corriente lo lleva
El Character nada cuando el agua le pasa del pecho; la cámara bajo el agua cambia la imagen
```
**Río:** **Añadir > Efectos visuales > Water Body River** (como el Water Body River de Unreal). En Detalles > **River**, cada punto tiene su lugar, **W** (ancho), **D** (profundidad) y **V** (velocidad); **Add Point** alarga el río. El color, la claridad y la espuma vienen del **Water Body** del mismo actor. El agua dibuja sus olas corriendo río abajo y corre más lenta en las orillas.

**Flotación:** en un actor con **Simulate Physics**, **+ Añadir > Buoyancy**. Flota en cualquier agua (lago, río, océano), se endereza con las olas y la corriente lo lleva. **Buoyancy** (1 = madera; 2 = una boya alta; por debajo de 0,5 se hunde), **Water Drag** (cuánto tira el agua) y **Pontoons** (1 o 5 puntos de apoyo).

**Nado:** un **Character** nada solo cuando el agua le pasa del pecho: sin gravedad, con la cabeza sobre el agua, más lento, y la corriente lo lleva. **Jump** nada hacia arriba; en aguas poco profundas camina más lento. **Swim Speed** (0 = 55 % de Max Walk Speed).

**Bajo el agua:** con la cámara en el agua, la imagen se vuelve azul verdosa (el rojo se pierde primero), más oscura cuanto más profundo, con una ligera ondulación. Con la lente en la superficie, la línea de flotación corta la pantalla. Para comparar sin ello: `REALENGINE_NO_UNDERWATER=1`.

Escenas de prueba: `river` (con Jugar) y `underwater`.
