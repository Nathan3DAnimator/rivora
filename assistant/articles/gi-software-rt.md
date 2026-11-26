id: gi-software-rt
kind: concept
title: Iluminação global sem placa RTX (ray tracing por software) | Global illumination without an RTX card (software ray tracing) | GI sin ray tracing por hardware
keys: gi sem rtx, gi sem ray tracing, sem ray tracing, no ray tracing, software ray tracing, ray tracing por software, placa sem rtx, gpu sem ray tracing, gtx, amd antiga, intel integrada, celular, android, quest, luz indireta no celular, indirect light on mobile, lumen software, no-rt, REALENGINE_NO_SWRT, gi no android
--- pt
```diagram steps
A GPU não tem ray tracing por hardware
A engine monta uma BVH de cada malha e uma das instâncias
O cache de superfície e as sondas traçam por software
A luz indireta segue as luzes que se movem, como numa RTX
```
Numa placa **sem ray tracing por hardware** (GTX, AMD antigas, Intel integradas, celulares Android, Quest), a iluminação global continua **dinâmica**: a engine traça os raios da GI por **software**, percorrendo estruturas próprias (BVH) na GPU. É automático.
- **O mesmo resultado:** a luz indireta fica a ~1% da versão por hardware. Uma luz que se move gera rebate; abrir uma porta muda a luz da sala.
- **O que muda sem RTX:** os reflexos traçados e as sombras por raio não existem; as sombras das luzes vêm dos mapas de sombra. O path tracer (tecla P) também é só por hardware.
- **Custo** (RTX 3060 com o ray tracing desligado, Médio): a casa a ~400 fps com 0,5 ms de GI; a RPG a ~130 fps com 2,6 ms. Num celular use os presets Baixo ou Médio.
- **Folhagem Masked** bloqueia a luz como se fosse opaca nesse modo.
- Para o comportamento antigo (só céu e lightmaps): `REALENGINE_NO_SWRT=1`. Para testar no PC: `--no-rt`.
--- en
```diagram steps
The GPU has no hardware ray tracing
The engine builds a BVH per mesh and one over the instances
The surface cache and the probes trace in software
Indirect light follows moving lights, as on an RTX
```
On a card **without hardware ray tracing** (GTX, older AMD, Intel integrated, Android phones, Quest), global illumination stays **dynamic**: the engine traces the GI's rays in **software**, walking its own structures (BVH) on the GPU. It's automatic.
- **The same result:** the indirect light is within ~1% of the hardware version. A moving light bounces; opening a door changes the room's light.
- **What changes without RTX:** traced reflections and ray traced shadows don't exist; lights' shadows come from shadow maps. The path tracer (P key) is hardware only too.
- **Cost** (RTX 3060 with ray tracing off, Medium): the house at ~400 fps with 0.5 ms of GI; the RPG at ~130 fps with 2.6 ms. On a phone use the Low or Medium presets.
- **Masked foliage** blocks light as if opaque in this mode.
- For the old behavior (sky and lightmaps only): `REALENGINE_NO_SWRT=1`. To test on a PC: `--no-rt`.
--- es
```diagram steps
La GPU no tiene ray tracing por hardware
El motor construye una BVH por malla y una sobre las instancias
La caché de superficie y las sondas trazan por software
La luz indirecta sigue a las luces en movimiento, como en una RTX
```
En una tarjeta **sin ray tracing por hardware** (GTX, AMD más antiguas, Intel integradas, teléfonos Android, Quest), la iluminación global sigue siendo **dinámica**: el motor traza los rayos de la GI por **software**, recorriendo sus propias estructuras (BVH) en la GPU. Es automático.
- **El mismo resultado:** la luz indirecta queda a ~1% de la versión por hardware. Una luz en movimiento rebota; abrir una puerta cambia la luz de la habitación.
- **Qué cambia sin RTX:** no existen los reflejos trazados ni las sombras por ray tracing; las sombras de las luces vienen de mapas de sombra. El path tracer (tecla P) también es solo por hardware.
- **Costo** (RTX 3060 con ray tracing apagado, Medio): la casa a ~400 fps con 0,5 ms de GI; el RPG a ~130 fps con 2,6 ms. En un teléfono usa los presets Bajo o Medio.
- **El follaje Masked** bloquea la luz como si fuera opaco en este modo.
- Para el comportamiento antiguo (solo cielo y lightmaps): `REALENGINE_NO_SWRT=1`. Para probar en un PC: `--no-rt`.
