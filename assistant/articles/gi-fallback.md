id: gi-fallback
kind: concept
title: GI de fallback: luz indireta sem raios (Baixo, Android, Quest) | GI fallback: indirect light without rays (Low, Android, Quest) | GI de respaldo: luz indirecta sin rayos (Bajo, Android, Quest)
keys: gi de fallback, gi fallback, gi sem raios, gi without rays, gi no celular, gi on mobile, android gi, quest gi, preset baixo gi, low preset gi, ao em volume, ao volume, oclusao do ceu, sky occlusion, luz rebatida do chao, ground bounce, rebote das luzes, light bounce, lampada sem gi, lamp without gi, sala escura no baixo, dark room on low, sala fechada preta, closed room black, rebote de luces, bounce map, embaixo do telhado escuro, dark under the roof, iluminacao indireta barata, cheap indirect light
--- pt
Quando não há lightmap, sondas nem cache de superfície (o preset **Baixo**, Android, Quest, GPUs sem ray tracing), a engine usa uma **GI de fallback** sozinha:
- **AO em volume**: uma grade de 128 × 128 × 32 m em volta da câmera, em células de 2 m, feita das caixas das malhas. Cada célula guarda quanto do céu ela vê (5 raios). Embaixo de um telhado, ao pé de um prédio ou entre muros, a luz do céu cai.
- **Luz rebatida do chão**: o sol bate no chão e volta para cima. O que olha para baixo ou para o lado recebe essa luz, e a parte do céu escondida é preenchida por ela (embaixo de um telheiro não fica preto, fica com a luz quente do chão ensolarado).
- **Rebote das lâmpadas**: as luzes Point, Spot e Rect (até 32 perto da câmera) iluminam as paredes, e essa luz se espalha pelo ar numa grade de 32 × 32 × 8 m com células de 50 cm, sem atravessar paredes. Uma sala fechada acesa por uma lâmpada, ou o cômodo ao lado visto pela porta, não fica mais preta.
- **Automático**: não há nada para ligar. Com sondas ou lightmap, eles têm prioridade.
- **Custo**: ~0,02 ms no passe principal (+0,05 ms com o rebote das lâmpadas); os volumes são refeitos numa thread quando a câmera anda, a cada 2 s ou quando uma luz muda (~4-8 ms fora do quadro).
- **Qualidade**: na cena de teste, a diferença para o RealGI (com raios) cai de 10,3% para 9,2%.

```diagram steps
sem lightmap, sem sondas | volume de AO em volta da câmera | céu escondido escurece | chão ensolarado rebate a luz | lâmpadas iluminam as paredes, a luz se espalha sem atravessá-las | indireta perto da do RealGI
```
--- en
When there's no lightmap, no probes and no surface cache (the **Low** preset, Android, Quest, GPUs without ray tracing), the engine uses a **GI fallback** by itself:
- **AO volume**: a 128 × 128 × 32 m grid around the camera, in 2 m cells, made from the meshes' boxes. Each cell keeps how much of the sky it sees (5 rays). Under a roof, at a building's foot or between walls, the sky light drops.
- **Ground bounce**: the sun hits the ground and comes back up. What faces down or sideways gets that light, and the hidden part of the sky is filled with it (under a carport it isn't black, it gets the sunlit ground's warm light).
- **Lamp bounce**: Point, Spot and Rect lights (up to 32 near the camera) light the walls, and that light spreads through the air in a 32 × 32 × 8 m grid of 50 cm cells, never through a wall. A closed room lit by a lamp, or the next room seen through a door, is no longer black.
- **Automatic**: nothing to turn on. With probes or a lightmap, those come first.
- **Cost**: ~0.02 ms in the main pass (+0.05 ms with the lamp bounce); the volumes are rebuilt on a thread when the camera moves, every 2 s or when a light changes (~4-8 ms off the frame).
- **Quality**: in the test scene, the difference to the RealGI (with rays) drops from 10.3% to 9.2%.

```diagram steps
no lightmap, no probes | AO volume around the camera | the hidden sky darkens | the sunlit ground bounces the light | lamps light the walls, the light spreads without crossing them | indirect light close to the RealGI
```
--- es
Cuando no hay lightmap, ni sondas, ni caché de superficie (el preset **Low**, Android, Quest, GPUs sin ray tracing), el motor usa por sí solo un **GI de respaldo**:
- **Volumen de AO**: una cuadrícula de 128 × 128 × 32 m alrededor de la cámara, en celdas de 2 m, hecha con las cajas de las mallas. Cada celda guarda cuánto cielo ve (5 rayos). Bajo un techo, al pie de un edificio o entre paredes, la luz del cielo baja.
- **Rebote del suelo**: el sol golpea el suelo y vuelve hacia arriba. Lo que mira hacia abajo o de lado recibe esa luz, y la parte oculta del cielo se rellena con ella (bajo una cochera no es negro, recibe la luz cálida del suelo iluminado por el sol).
- **Rebote de las lámparas**: las luces Point, Spot y Rect (hasta 32 cerca de la cámara) iluminan las paredes, y esa luz se reparte por el aire en una cuadrícula de 32 × 32 × 8 m con celdas de 50 cm, sin atravesar paredes. Una sala cerrada iluminada por una lámpara, o la habitación de al lado vista por la puerta, ya no queda negra.
- **Automático**: nada que activar. Con sondas o un lightmap, esos tienen prioridad.
- **Costo**: ~0,02 ms en el pase principal (+0,05 ms con el rebote de las lámparas); los volúmenes se reconstruyen en un hilo cuando la cámara se mueve, cada 2 s o cuando cambia una luz (~4-8 ms fuera del fotograma).
- **Calidad**: en la escena de prueba, la diferencia con el RealGI (con rayos) baja de 10,3% a 9,2%.

```diagram steps
sin lightmap, sin sondas | volumen de AO alrededor de la cámara | el cielo oculto se oscurece | el suelo iluminado por el sol rebota la luz | las lámparas iluminan las paredes, la luz se reparte sin atravesarlas | luz indirecta cercana al RealGI
```
