id: udim
kind: howto
title: Texturas UDIM: vários tiles numa malha | UDIM textures: many tiles on one mesh | Texturas UDIM: varios tiles en una malla
keys: udim, udims, tile udim, tiles, 1001, 1002, 1011, uvtile, u1_v1, mari, substance painter, texturas em tiles, tiled texture, texturas en mosaico, personagem 8k, character 8k, personaje 8k, varios mapas uv, multiple uv tiles, rudim, importar udim, import udim, importar pasta udim, import udim folder, usar como udim, use as udim, texture sample udim, virtual texture, textura virtual, costura entre tiles, seam between tiles, costura udim, udim seam, streaming por tile, tile streaming, bc5 normal, bc7
--- pt
Um personagem ou um cenário grande pinta cada parte num **tile** do UV: o 1001 cobre o UV 0..1, o 1002 fica à direita (u 1..2), o 1011 fica acima (v 1..2), dez por linha. Cada tile tem a própria imagem (`Corpo.1001.png`, `Corpo.1002.png`...), então o modelo inteiro ganha a resolução de vários mapas.
1. **Importar**: solte um tile do Explorer no Content Browser (os outros tiles do conjunto vêm juntos) ou use o clique direito > **Importar Pasta UDIM...**. Sai um asset **.rudim**: cada tile comprimido em **BC7** (normal maps em **BC5**, pelo nome: *normal*, *nrm*, *_n*) e com os mips prontos, lido direto do disco.
2. **Material**: arraste o .rudim para o grafo (vira um **Image Texture**) e ligue como uma imagem comum. O tile vem da UV de cada pixel; não há nó novo. Uma imagem `<nome>.1001.png` com outros tiles ao lado mostra o botão **Usar como UDIM** nos Details do nó. Normal map: Image Texture em *Non-Color* > Normal Map.
3. **Sem emendas**: cada tile é lido com a derivada do UV inteiro (o mip não pula na borda do tile) e preso à própria borda (nada vaza do lado oposto). O material é assado por tile.
4. **Streaming por tile**: só os tiles que a câmera vê sobem de resolução; os que saem de vista por 2 s descem ao mínimo (64), dentro do orçamento de texturas. Veja no **Debug Viewer** (F10) > Texture Streaming: cada tile com o tamanho atual e o pedido.
5. **Importadores**: glTF, FBX, OBJ e `.tlend` reconhecem UDIM pelo nome do arquivo (`.1001.`, `_1001.`, `<UDIM>`, `_u1_v1`) e criam o .rudim sozinhos.

```diagram steps
Pintar em tiles (1001, 1002, 1011...) | Importar a pasta ou um tile: .rudim (BC7 / BC5, mips) | Image Texture com o .rudim | Assado por tile, sem emenda | Streaming: só os tiles vistos
```
```diagram bars MB
Todos os tiles cheios (36 tiles da cena de teste): 358
Só os vistos (câmera andando na estrada): 23
```
--- en
A character or a large set paints each part on a UV **tile**: 1001 covers UV 0..1, 1002 is to its right (u 1..2), 1011 above it (v 1..2), ten a row. Each tile has an image of its own (`Body.1001.png`, `Body.1002.png`...), so the whole model gets the resolution of many maps.
1. **Import**: drop one tile from Explorer into the Content Browser (the set's other tiles come with it) or right click > **Import UDIM Folder...**. It makes a **.rudim** asset: each tile compressed to **BC7** (normal maps to **BC5**, by their name: *normal*, *nrm*, *_n*) with its mips, read straight from the disk.
2. **Material**: drag the .rudim onto the graph (it becomes an **Image Texture**) and link it like any image. The tile comes from each pixel's UV; there is no new node. An image `<name>.1001.png` with other tiles beside it shows the **Use as UDIM** button in the node's Details. Normal map: Image Texture in *Non-Color* > Normal Map.
3. **No seams**: each tile is read with the whole UV's derivatives (the mip doesn't jump at the tile's edge) and clamped to its own edge (nothing bleeds from the far side). The material is baked per tile.
4. **Streaming per tile**: only the tiles the camera sees climb; the ones out of view for 2 s go down to the least (64), within the texture budget. See it in the **Debug Viewer** (F10) > Texture Streaming: each tile's size now and wanted.
5. **Importers**: glTF, FBX, OBJ and `.tlend` recognize UDIM by the file's name (`.1001.`, `_1001.`, `<UDIM>`, `_u1_v1`) and make the .rudim by themselves.

```diagram steps
Paint in tiles (1001, 1002, 1011...) | Import the folder or one tile: .rudim (BC7 / BC5, mips) | Image Texture with the .rudim | Baked per tile, no seam | Streaming: only the tiles seen
```
--- es
Un personaje o un conjunto grande pinta cada parte en un **tile** UV: el 1001 cubre el UV 0..1, el 1002 está a su derecha (u 1..2), el 1011 encima (v 1..2), diez por fila. Cada tile tiene su propia imagen (`Body.1001.png`, `Body.1002.png`...), así que todo el modelo obtiene la resolución de muchos mapas.
1. **Importar**: suelta un tile del Explorador en el Explorador de contenidos (los demás tiles del conjunto vienen con él) o clic derecho > **Import UDIM Folder...**. Crea un asset **.rudim**: cada tile comprimido a **BC7** (los mapas de normales a **BC5**, por su nombre: *normal*, *nrm*, *_n*) con sus mips, leído directamente del disco.
2. **Material**: arrastra el .rudim al grafo (se convierte en un **Image Texture**) y enlázalo como cualquier imagen. El tile sale del UV de cada píxel; no hay ningún nodo nuevo. Una imagen `<nombre>.1001.png` con otros tiles al lado muestra el botón **Use as UDIM** en los Detalles del nodo. Mapa de normales: Image Texture en *Non-Color* > Normal Map.
3. **Sin costuras**: cada tile se lee con las derivadas de todo el UV (el mip no salta en el borde del tile) y se limita a su propio borde (nada se cuela del otro lado). El material se hornea por tile.
4. **Streaming por tile**: solo suben los tiles que ve la cámara; los que llevan 2 s fuera de la vista bajan al mínimo (64), dentro del presupuesto de texturas. Míralo en el **Debug Viewer** (F10) > Texture Streaming: el tamaño de cada tile ahora y el deseado.
5. **Importadores**: glTF, FBX, OBJ y `.tlend` reconocen UDIM por el nombre del archivo (`.1001.`, `_1001.`, `<UDIM>`, `_u1_v1`) y crean el .rudim por sí solos.

```diagram steps
Pinta en tiles (1001, 1002, 1011...) | Importa la carpeta o un tile: .rudim (BC7 / BC5, mips) | Image Texture con el .rudim | Horneado por tile, sin costura | Streaming: solo los tiles vistos
```
