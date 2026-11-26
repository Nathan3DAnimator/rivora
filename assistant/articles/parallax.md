id: parallax
kind: concept
title: Parallax: relevo sem polígonos | Parallax: depth without polygons | Parallax occlusion
keys: parallax, parallax occlusion, pom, relevo no material, relevo sem poligonos, pedras no material, relevo, profundidade, depth, displacement, deslocamento, height map, mapa de altura, altura, height, calcamento, cobblestones, tijolos, bricks, pedras, stones, rejunte, mortar, superficie afundada, sunken surface, microdeslocamento, micro displacement, geometry tessellation, tesselacao de geometria
--- pt
```diagram steps
Material Opaque: um nó Displacement
Height: a altura (0 fundo, 1 em cima), Scale: a profundidade
Displacement ligado na entrada Displacement da Material Output
A superfície parece afundar onde a altura é baixa
```
O **parallax occlusion** faz uma superfície plana parecer ter relevo: pedras, tijolos, rejunte afundado. Cada pixel procura, ao longo do olhar, onde o raio encontra o mapa de altura, e lê o material ali.
- No material (**Blend Mode Opaque**), ligue um nó **Displacement** (Shift+A > Vetor) na entrada **Displacement** da **Material Output**. Na **Height** do Displacement vai a altura: 0 no fundo, 1 em cima. A **Scale** é a profundidade (1 = 5% de uma repetição do UV).
- Use a mesma altura num **Bump** ligado na Normal: o parallax dá a profundidade e o Bump dá a luz nos lados das pedras.
- O exemplo **M_ParallaxCobble** faz um calçamento com dois Voronoi (F2 − F1).
- Custa algumas leituras de textura a mais, só nos pixels desse material (cerca de 0,15 ms com metade da tela coberta no Épico).
- **Limites**: a borda da malha continua reta (não recorta a silhueta), e materiais Masked ou translúcidos não fazem parallax.
- **Relevo de verdade** (a silhueta muda): veja Geometry Tessellation.
--- en
```diagram steps
Opaque material: a Displacement node
Height: the height (0 bottom, 1 top), Scale: the depth
Displacement into the Material Output's Displacement input
The surface looks sunk where the height is low
```
**Parallax occlusion** makes a flat surface look like it has depth: stones, bricks, sunken mortar. Each pixel searches along the view for where the ray meets the height map, and reads the material there.
- In the material (**Blend Mode Opaque**), link a **Displacement** node (Shift+A > Vector) into the **Material Output**'s **Displacement** input. The Displacement's **Height** is the height: 0 at the bottom, 1 on top. **Scale** is the depth (1 = 5% of a UV repeat).
- Feed the same height into a **Bump** on the Normal: the parallax gives the depth, the Bump the light on the stones' sides.
- The **M_ParallaxCobble** example makes cobblestones from two Voronoi (F2 − F1).
- It costs a few more texture reads, only on that material's pixels (about 0.15 ms with half the screen covered at Epic).
- **Limits**: the mesh's edge stays straight (no cut silhouette), and Masked or translucent materials don't do parallax.
- **Real relief** (the silhouette changes): see Geometry Tessellation.
--- es
```diagram steps
Material opaco: un nodo Displacement
Height: la altura (0 abajo, 1 arriba), Scale: la profundidad
Displacement a la entrada Displacement del Material Output
La superficie parece hundida donde la altura es baja
```
La **parallax occlusion** hace que una superficie plana parezca tener profundidad: piedras, ladrillos, argamasa hundida. Cada píxel busca a lo largo de la vista dónde el rayo encuentra el mapa de alturas, y lee el material allí.
- En el material (**Blend Mode Opaque**), conecta un nodo **Displacement** (Shift+A > Vector) a la entrada **Displacement** del **Material Output**. El **Height** del Displacement es la altura: 0 abajo, 1 arriba. **Scale** es la profundidad (1 = 5% de una repetición de UV).
- Pon la misma altura en un **Bump** sobre la Normal: el parallax da la profundidad, el Bump la luz en los costados de las piedras.
- El ejemplo **M_ParallaxCobble** hace adoquines con dos Voronoi (F2 − F1).
- Cuesta unas lecturas de textura más, solo en los píxeles de ese material (cerca de 0,15 ms con la mitad de la pantalla cubierta en Épico).
- **Límites**: el borde de la malla sigue recto (sin silueta recortada), y los materiales Masked o translúcidos no hacen parallax.
- **Relieve real** (la silueta cambia): mira Geometry Tessellation.
