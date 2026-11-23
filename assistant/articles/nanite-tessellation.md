id: nanite-tessellation
kind: howto
title: Geometry Tessellation: deslocamento de verdade | Geometry Tessellation: real displacement
keys: tesselação, tesselacao, tessellation, nanite tessellation, deslocamento, displacement, displacement map, mapa de altura, height map, heightmap, relevo, relevo de verdade, silhueta, subdivisão, subdivisao, subdividir, microdeslocamento, micro displacement, enable tessellation, displacement magnitude, displacement center, parallax, pedras, rochas, chão de pedra, calçamento, cobblestone, teselación, desplazamiento, mapa de alturas, relieve, subdivisión, geometry tessellation, tesselacao de geometria
--- pt
```diagram steps
Altura no Displacement da Material Output | Detalhes do material > Nanite > Ativar Tesselação | Magnitude (cm) e Centro | Malha com LOD de clusters (16 mil triângulos ou mais) | A GPU subdivide pelo tamanho na tela e desloca
```
**Geometry Tessellation** (como a da Unreal 5.4+) move a superfície de verdade pela altura do material: a silhueta, as sombras e o contato mudam, não só o sombreamento (o parallax só finge o relevo dentro da superfície).

1. No material **Opaco**, ligue uma altura (uma textura, um Noise, um Voronoi) num nó **Displacement** e ele na entrada **Displacement** da **Material Output**.
2. Nos **Detalhes** do material, seção **Nanite**: marque **Ativar Tesselação**.
3. **Magnitude do Deslocamento**: quantos cm a faixa inteira da altura (0 a 1) move a superfície (cm da malha: a escala do ator também multiplica). **Centro do Deslocamento**: a altura que fica no lugar (o Midlevel do Blender): abaixo dela afunda, acima sobe.
4. Use numa malha com **LOD de clusters** (16 mil triângulos ou mais, importada). Perto da câmera os triângulos dela são subdivididos na GPU até uma aresta a cada **4 px** (Épico) e empurrados ao longo da normal. Longe, só os vértices do LOD se movem.

**Sem rachaduras**: cada ponto de uma aresta é calculado só pela aresta (as duas pontas em ordem fixa), então os dois triângulos dela chegam ao mesmo número, mesmo com subdivisões diferentes e nas costuras de UV. O teste `tess_seams` confere aresta por aresta.

**Escala** (Configurações > Escalabilidade): Cinemático 2 px, Épico 4 px, Alto 6 px, Médio 10 px por microaresta; **Baixo** só desloca os vértices (nada subdividido). Há um **orçamento** de microtriângulos por quadro: passou dele, as microarestas crescem sozinhas. O **Debug Viewer** (F10) > **Tesselação** mostra o custo e os microtriângulos.

**Sombras e raios**: os mapas de sombra (sol e luzes locais) veem o deslocamento nos vértices; as sombras e reflexos por raio veem a malha base, com o início do raio afastado o bastante para não se autossombrear.

Custo medido (100 rochas, RTX 3060, 1440p, GPU em ms):
```diagram bars ms
Sem deslocamento: 4.97
Só vértices (Baixo): 5.45
Alto (6 px): 6.23
Épico (4 px): 6.81
Cinemático (2 px): 8.25
```
(Desde o 0.43, Q22: Épico ~6,5 ms, o microvértice num shader próprio.) Por código ou Blueprint, as variáveis de console: `r.Tessellation` (0/1), `r.Tessellation.DicingRate` (px), `r.Tessellation.MaxFactor`, `r.Tessellation.Budget` (milhões).
```blueprint
Event BeginPlay
  -> Set Console Variable ( Name = "r.Tessellation.DicingRate", Value = 6 )
```
--- en
```diagram steps
A height into the Material Output's Displacement | Material Details > Nanite > Enable Tessellation | Magnitude (cm) and Center | A mesh with a cluster LOD (16 thousand triangles or more) | The GPU subdivides by the size on screen and displaces
```
**Geometry Tessellation** (like Unreal 5.4+'s) moves the surface for real by the material's height: the silhouette, the shadows and the contact change, not just the shading (parallax only fakes the relief inside the surface).

1. In an **Opaque** material, connect a height (a texture, a Noise, a Voronoi) to a **Displacement** node and that to the **Material Output**'s **Displacement** input.
2. In the material's **Details**, **Nanite** section: check **Enable Tessellation**.
3. **Displacement Magnitude**: how many cm the height's whole range (0 to 1) moves the surface (cm of the mesh: the actor's scale scales it). **Displacement Center**: the height that stays in place (Blender's Midlevel): below it sinks, above it rises.
4. Use it on a mesh with a **cluster LOD** (16 thousand triangles or more, imported). Near the camera its triangles are subdivided on the GPU down to an edge every **4 px** (Epic) and pushed along the normal. Far away, only the LOD's vertices move.

**No cracks**: each point of an edge is computed from that edge alone (its two ends in a fixed order), so both triangles along it get the same numbers, even with different subdivisions and across UV seams. The `tess_seams` test checks it edge by edge.

**Scalability** (Settings > Scalability): Cinematic 2 px, Epic 4 px, High 6 px, Medium 10 px a micro edge; **Low** only displaces the vertices (nothing subdivided). There is a **budget** of micro triangles a frame: past it, the micro edges grow by themselves. The **Debug Viewer** (F10) > **Tessellation** shows the cost and the micro triangles.

**Shadows and rays**: the shadow maps (sun and local lights) see the displacement at the vertices; ray traced shadows and reflections see the base mesh, the ray starting far enough out not to shadow itself.

Measured cost (100 rocks, RTX 3060, 1440p, GPU ms):
```diagram bars ms
No displacement: 4.97
Vertices only (Low): 5.45
High (6 px): 6.23
Epic (4 px): 6.81
Cinematic (2 px): 8.25
```
(Since 0.43, Q22: Epic ~6.5 ms, the micro vertex in a shader of its own.) From code or a Blueprint, the console variables: `r.Tessellation` (0/1), `r.Tessellation.DicingRate` (px), `r.Tessellation.MaxFactor`, `r.Tessellation.Budget` (millions).
```blueprint
Event BeginPlay
  -> Set Console Variable ( Name = "r.Tessellation.DicingRate", Value = 6 )
```
--- es
```diagram steps
Una altura en el Displacement de la Material Output | Detalles del material > Nanite > Activar Teselación | Magnitud (cm) y Centro | Una malla con LOD de clusters (16 mil triángulos o más) | La GPU subdivide por el tamaño en pantalla y desplaza
```
**Geometry Tessellation** (como la de Unreal 5.4+) mueve la superficie de verdad según la altura del material: la silueta, las sombras y el contacto cambian, no solo el sombreado (el parallax solo finge el relieve dentro de la superficie).

1. En un material **Opaco**, conecta una altura (una textura, un Noise, un Voronoi) a un nodo **Displacement** y este a la entrada **Displacement** de la **Material Output**.
2. En los **Detalles** del material, sección **Nanite**: marca **Activar Teselación** (Enable Tessellation).
3. **Displacement Magnitude**: cuántos cm mueve la superficie todo el rango de la altura (0 a 1), en cm de la malla (la escala del actor también la multiplica). **Displacement Center**: la altura que queda en su lugar (el Midlevel de Blender).
4. Úsalo en una malla con **LOD de clusters** (16 mil triángulos o más, importada). Cerca de la cámara sus triángulos se subdividen en la GPU hasta una arista cada **4 px** (Épico) y se empujan a lo largo de la normal. Lejos, solo se mueven los vértices del LOD.

**Sin grietas**: cada punto de una arista se calcula solo con esa arista, así que los dos triángulos que la comparten llegan al mismo número, incluso en las costuras de UV (la prueba `tess_seams` lo comprueba). **Escalabilidad**: Cinemático 2 px, Épico 4, Alto 6, Medio 10; **Bajo** solo desplaza los vértices. Un **presupuesto** de microtriángulos por cuadro ajusta el detalle solo. **Debug Viewer** (F10) > **Teselación** muestra el costo. Las sombras por mapa ven el desplazamiento en los vértices; las de rayos ven la malla base. Variables de consola: `r.Tessellation`, `r.Tessellation.DicingRate`, `r.Tessellation.MaxFactor`, `r.Tessellation.Budget`.
```blueprint
Event BeginPlay
  -> Set Console Variable ( Name = "r.Tessellation.DicingRate", Value = 6 )
```
