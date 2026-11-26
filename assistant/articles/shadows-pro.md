id: shadows-pro
kind: concept
title: Sombras: virtuais, por raio, PCSS | Shadows: virtual, ray traced, PCSS | Sombras: virtuales, por rayos, PCSS
keys: mapa de sombra da luz pontual, point light shadow map, cube shadow map, sombra sem ray tracing, sombra no baixo, sombra aparece do nada, shadow pops in, sombra surge de perto, transicao de sombra, shadow transition, fade de sombra, shadow fade, sombra, sombras, shadow, virtual shadow maps, vsm, pcss, sombra suave, sombra serrilhada, sombra pixelada, sombra de longe, ray traced shadows
--- pt
- **Sol:** sombras virtuais em clipmap com cache (só redesenha o que mudou), suavidade física (PCSS) e raio para o que está perto do bloqueador.
- **Luzes locais:** por raio quando há ray tracing (Médio para cima); luzes de área com penumbra filtrada.
- **Luzes locais no Baixo ou sem ray tracing:** mapas de sombra num atlas. A pontual desenha as 6 faces de um cubo, a spot uma face. As mais importantes na tela ganham mapa (Baixo 8, Médio 16, Alto 24, Épico 32). O mapa fica em cache e só redesenha o que mudou. O filtro é suave (grade de comparações bilineares) e sem acne.
- **Masked** (folhas, grades): a sombra respeita o recorte do material.
- Sombra pixelada ou sumindo de longe: suba a escalabilidade de sombras ou aproxime a câmera; luz Static com lightmap custa menos.
- **Transições suaves:** ao chegar perto ou se afastar, a sombra passa de um nível de detalhe para o outro aos poucos (fade nos últimos 30% de cada nível) e a troca mapa → raio é uma rampa; no fim do alcance a sombra some aos poucos. Nada aparece "do nada" no personagem.
--- en
- **Sun:** cached clipmap virtual shadows (only what changed is redrawn), physical softness (PCSS) and rays near the blocker.
- **Local lights:** ray traced when there's ray tracing (Medium and up); area lights with filtered penumbra.
- **Local lights on Low or without ray tracing:** shadow maps in an atlas. A point light draws the 6 faces of a cube, a spot one face. The most important on screen get a map (Low 8, Medium 16, High 24, Epic 32). The map is cached and only what changed is redrawn. The filter is smooth (a grid of bilinear compares) and acne-free.
- **Masked** (leaves, grates): the shadow follows the material's cutout.
- Pixelated shadow or fading far away: raise the shadows' scalability or bring the camera closer; a Static light with a lightmap costs less.
- **Smooth transitions:** walking up or away, a shadow goes from one level of detail to the next gradually (a fade over each level's last 30%) and the map-to-ray switch is a ramp; at the end of the range the shadow fades out. Nothing pops in on a character.
--- es
- **Sol:** sombras virtuales con clipmap en caché (solo se redibuja lo que cambió), suavidad física (PCSS) y rayos cerca del bloqueador.
- **Luces locales:** por rayos cuando hay ray tracing (Medium y superior); luces de área con penumbra filtrada.
- **Luces locales en Low o sin ray tracing:** mapas de sombras en un atlas. Una luz puntual dibuja las 6 caras de un cubo, un foco una cara. Las más importantes en pantalla reciben un mapa (Low 8, Medium 16, High 24, Epic 32). El mapa se guarda en caché y solo se redibuja lo que cambió. El filtro es suave (una cuadrícula de comparaciones bilineales) y sin acné.
- **Masked** (hojas, rejillas): la sombra sigue el recorte del material.
- Sombra pixelada o que se desvanece a lo lejos: sube la escalabilidad de las sombras o acerca la cámara; una luz Static con lightmap cuesta menos.
- **Transiciones suaves:** al acercarte o alejarte, una sombra pasa de un nivel de detalle al siguiente gradualmente (un fundido en el último 30% de cada nivel) y el cambio de mapa a rayo es una rampa; al final del alcance la sombra se desvanece. Nada aparece de golpe sobre un personaje.
