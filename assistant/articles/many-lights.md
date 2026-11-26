id: many-lights
kind: concept
title: Muitas luzes com sombra (como o MegaLights) | Many shadowed lights (like MegaLights) | Muchas luces con sombra (como MegaLights)
keys: muitas luzes, megalights, mega lights, milhares de luzes, sombra de varias luzes, luz atravessa parede, lampadas, tochas, cidade a noite, many lights, stochastic, restir, restir di, reservatorio, reservoir, sombra suave
--- pt
```diagram bars ms
10 mil luzes suaves, antes: 28.9
10 mil luzes suaves, agora: 10.2
Todas traçadas (referência): 71.0
```
Cada pixel sorteia as luzes que mais pesam e traça raios só para elas: o custo das sombras não cresce com o número de luzes (como o MegaLights da Unreal).
- Liga sozinho com ray tracing e qualquer luz local com **Cast Shadows**. Nada para configurar.
- Antes, só as 8 primeiras luzes de cada lugar tinham sombra, e a luz das outras atravessava paredes. Agora todas têm.
- Raios por luz sorteada vêm da escalabilidade (Alto 1, Épico 2, Cinemático 4). Um filtro e um histórico tiram o ruído.
- Para comparar: `REALENGINE_NO_SHADOW_FILTER=1` volta ao caminho antigo; `REALENGINE_SHADOW_BUDGET=64` traça todas (a referência).
- Veja também: [Luzes](article:lights), [Sombras](article:shadows-pro).
- **No Fotorrealista (ReSTIR DI):** com o denoiser unificado ligado, cada pixel guarda a luz que sorteou e reaproveita a do quadro anterior (só se ela estava visível): os raios param de ir para luzes atrás de paredes. Traça uma luz em vez de duas (~5% mais rápido, com 10 mil luzes) e o erro contra o path tracing cai um pouco. `REALENGINE_NO_RESTIR=1` volta às duas luzes sorteadas.
--- en
```diagram bars ms
10k soft lights, before: 28.9
10k soft lights, now: 10.2
All traced (reference): 71.0
```
Each pixel draws the lights that matter most and traces rays only for them: the shadows' cost doesn't grow with the number of lights (like Unreal's MegaLights).
- On by itself with ray tracing and any local light with **Cast Shadows**. Nothing to set up.
- Before, only the first 8 lights of each place had shadows, and the others' light went through walls. Now all do.
- Rays per drawn light come from the scalability (High 1, Epic 2, Cinematic 4). A filter and a history remove the noise.
- To compare: `REALENGINE_NO_SHADOW_FILTER=1` goes back to the old path; `REALENGINE_SHADOW_BUDGET=64` traces them all (the reference).
- **In Photoreal (ReSTIR DI):** with the unified denoiser on, each pixel keeps the light it picked and reuses last frame's (only if it was visible): rays stop going to lights behind walls. It traces one light instead of two (~5% faster with 10,000 lights) and the error against the path tracer drops a little. `REALENGINE_NO_RESTIR=1` goes back to the two picked lights.
- See also: [Lights](article:lights), [Shadows](article:shadows-pro).
--- es
```diagram bars ms
10k luces suaves, antes: 28.9
10k luces suaves, ahora: 10.2
Todo trazado (referencia): 71.0
```
Cada píxel dibuja las luces que más importan y traza rayos solo para ellas: el costo de las sombras no crece con el número de luces (como el MegaLights de Unreal).
- Activado por sí solo con ray tracing y cualquier luz local con **Cast Shadows**. Nada que configurar.
- Antes, solo las primeras 8 luces de cada lugar tenían sombras, y la luz de las demás atravesaba las paredes. Ahora todas las tienen.
- Los rayos por luz dibujada vienen de la escalabilidad (High 1, Epic 2, Cinematic 4). Un filtro y un historial eliminan el ruido.
- Para comparar: `REALENGINE_NO_SHADOW_FILTER=1` vuelve al camino antiguo; `REALENGINE_SHADOW_BUDGET=64` las traza todas (la referencia).
- **En Photoreal (ReSTIR DI):** con el denoiser unificado activado, cada píxel guarda la luz que eligió y reutiliza la del fotograma anterior (solo si era visible): los rayos dejan de ir a luces detrás de paredes. Traza una luz en vez de dos (~5% más rápido con 10.000 luces) y el error frente al path tracer baja un poco. `REALENGINE_NO_RESTIR=1` vuelve a las dos luces elegidas.
- Mira también: [Luces](article:lights), [Sombras](article:shadows-pro).
