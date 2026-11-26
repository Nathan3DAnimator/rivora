id: asset-details
kind: howto
title: Detalhes dos visualizadores: LODs, Build, textura, malha | Viewer Details: LODs, Build, texture, mesh | Detalles de los visores: LODs, Build, textura, malla
keys: lod, lods, level of detail, nivel de detalhe, nível de detalhe, reduzir triangulos, reduzir triângulos, reduce triangles, reduction, reducao, redução, simplificar, simplify, numero de lods, número de lods, number of lods, lod settings, configuracoes de lod, configurações de lod, build settings, recalcular normais, recompute normals, angulo de suavizacao, ângulo de suavização, smoothing angle, escala de build, build scale, estatisticas, estatísticas, statistics, memoria, memória, memory, lod bias, compressao, compressão, compression, bc7, mips, buscar nos detalhes, search details, buscar ossos, search bones, morph targets, physics asset, visualizador, viewer, static mesh editor, texture editor, skeletal mesh editor, sound wave
--- pt
Os visualizadores (**Static Mesh**, **Textura**, **Skeletal Mesh**, **Esqueleto**, **Animação** e **Sound Wave**) têm o painel de **Detalhes** como o da Unreal: uma **busca** no topo e **seções que recolhem**. Digite "lod", "normal" ou "memória" e só as seções com isso aparecem, abertas.

```diagram steps
Duplo clique no asset | Configurações de LOD | Número de LODs e % de cada um | Aplicar LODs | Estatísticas mostram os triângulos
```
## Static Mesh
- **Detalhes do Asset**: caminho, triângulos, vértices, tamanho, canais de UV (lightmap), cores por vértice, tamanho no disco.
- **Configurações de LOD**: **Número de LODs** (1 a 4), a **% dos triângulos** de cada LOD em relação ao LOD 0 e o **Mínimo de Triângulos**. **Aplicar LODs** refaz os LODs a partir do LOD 0 e salva na malha: os atores do nível já usam. **LODs Automáticos** volta à regra da engine (cada LOD um quarto do anterior).
- **Configurações de Build**: **Recalcular Normais** pelo **Ângulo de Suavização** e **Aplicar Escala** (muda o tamanho da própria malha). Isso fica gravado no arquivo; reimportar traz o original.
- **Estatísticas**: triângulos, % e vértices de cada LOD, a memória de cada um na GPU e o erro em cm.
- **Slots de Material**, **Sockets** e **Prévia** (grade, enquadrar). Os menus da viewport mudam o modo (Wireframe, UV, Normais...) e forçam um LOD.

## Textura
- **Textura**: tamanho importado e exibido, formato do arquivo e na GPU (BC7 ou RGBA8), mips e **Tamanho do Recurso**.
- **Configurações da Textura**: espaço de cor, **LOD Bias** (descarta os mips maiores: metade do tamanho por passo), tamanho máximo, **compressão**, filtro e repetição. **Canais** (R, G, B, A e o mip), **Ações** (Reimportar) e **Visualização** (zoom).

## Skeletal Mesh, Esqueleto, Animação e Som
- **Skeletal Mesh**: ossos, sockets, skins, partes, morph targets e triângulos; **LODs das skins** refeitos com o número e a % (os pesos dos ossos continuam certos); materiais; **pesos padrão dos morph targets**; **Physics Asset e IK Rig** (abrir ou criar); o osso selecionado. A **Árvore do Esqueleto** tem **Buscar Ossos**.
- **Animação**: duração, quadros, taxa, trilhas, notifies, curvas, memória e **Repetir**.
- **Sound Wave**: duração, taxa de amostragem, canais, formato, memória, pico e RMS, além das seções Som e Configurações.

```diagram bars ms
Refazer 3 LODs de uma malha de 8192 triângulos: 37
```
--- en
The viewers (**Static Mesh**, **Texture**, **Skeletal Mesh**, **Skeleton**, **Animation** and **Sound Wave**) have a **Details** panel like Unreal's: a **search** on top and **sections that fold**. Type "lod", "normal" or "memory" and only the sections that have it show, open.

```diagram steps
Double-click the asset | LOD Settings | Number of LODs and each one's % | Apply LOD Settings | Statistics show the triangles
```
## Static Mesh
- **Asset Details**: path, triangles, vertices, size, UV channels (lightmap), vertex colors, size on disk.
- **LOD Settings**: **Number of LODs** (1 to 4), each LOD's **% of the triangles** of LOD 0 and the **Min Triangles**. **Apply LOD Settings** makes the LODs again from LOD 0 and saves them in the mesh: the level's actors use them at once. **Automatic LODs** goes back to the engine's rule (each LOD a quarter of the one before).
- **Build Settings**: **Recompute Normals** by the **Smoothing Angle** and **Apply Scale** (resizes the mesh itself). Written into the file; a reimport brings the original back.
- **Statistics**: each LOD's triangles, % and vertices, its GPU memory and its error in cm.
- **Material Slots**, **Sockets** and **Preview** (grid, frame). The viewport's menus change the view mode (Wireframe, UV, Normals...) and force a LOD.

## Texture
- **Texture**: imported and displayed size, the file's format and the GPU's (BC7 or RGBA8), mips and **Resource Size**.
- **Texture Settings**: color space, **LOD Bias** (drops the largest mips: half the size per step), max size, **compression**, filter and wrap. **Channels** (R, G, B, A and the mip), **Actions** (Reimport) and **View** (zoom).

## Skeletal Mesh, Skeleton, Animation and Sound
- **Skeletal Mesh**: bones, sockets, skins, parts, morph targets and triangles; the **skins' LODs** made again with the number and the % (the bones' weights stay right); materials; the **morph targets' default weights**; **Physics Asset and IK Rig** (open or create); the selected bone. The **Skeleton Tree** has **Search Bones**.
- **Animation**: length, frames, rate, tracks, notifies, curves, memory and **Loop**.
- **Sound Wave**: duration, sample rate, channels, format, memory, peak and RMS, besides the Sound and Settings sections.

```diagram bars ms
Making 3 LODs of an 8192-triangle mesh again: 37
```
--- es
Los visores (**Static Mesh**, **Texture**, **Skeletal Mesh**, **Skeleton**, **Animation** y **Sound Wave**) tienen un panel **Detalles** como el de Unreal: una **búsqueda** arriba y **secciones que se pliegan**. Escribe "lod", "normal" o "memory" y solo se muestran, abiertas, las secciones que lo tienen.

```diagram steps
Doble clic en el asset | LOD Settings | Número de LODs y el % de cada uno | Apply LOD Settings | Las estadísticas muestran los triángulos
```
## Static Mesh
- **Asset Details**: ruta, triángulos, vértices, tamaño, canales UV (lightmap), colores de vértice, tamaño en disco.
- **LOD Settings**: **Number of LODs** (de 1 a 4), el **% de los triángulos** del LOD 0 para cada LOD y los **Min Triangles**. **Apply LOD Settings** vuelve a crear los LODs a partir del LOD 0 y los guarda en la malla: los actores del nivel los usan de inmediato. **Automatic LODs** vuelve a la regla del motor (cada LOD, la cuarta parte del anterior).
- **Build Settings**: **Recompute Normals** según el **Smoothing Angle** y **Apply Scale** (cambia el tamaño de la propia malla). Se escribe en el archivo; una reimportación trae de vuelta el original.
- **Statistics**: los triángulos, el % y los vértices de cada LOD, su memoria de GPU y su error en cm.
- **Material Slots**, **Sockets** y **Preview** (cuadrícula, encuadre). Los menús de la ventana gráfica cambian el modo de vista (Wireframe, UV, Normals...) y fuerzan un LOD.

## Texture
- **Texture**: tamaño importado y mostrado, el formato del archivo y el de la GPU (BC7 o RGBA8), mips y **Resource Size**.
- **Texture Settings**: espacio de color, **LOD Bias** (descarta los mips más grandes: la mitad del tamaño por paso), tamaño máximo, **compresión**, filtro y wrap. **Channels** (R, G, B, A y el mip), **Actions** (Reimport) y **View** (zoom).

## Skeletal Mesh, Skeleton, Animation y Sound
- **Skeletal Mesh**: huesos, sockets, skins, partes, morph targets y triángulos; los **LODs de los skins** rehechos con el número y el % (los pesos de los huesos se mantienen correctos); materiales; los **pesos por defecto de los morph targets**; **Physics Asset e IK Rig** (abrir o crear); el hueso seleccionado. El **Skeleton Tree** tiene **Search Bones**.
- **Animation**: duración, fotogramas, frecuencia, pistas, notifies, curvas, memoria y **Loop**.
- **Sound Wave**: duración, frecuencia de muestreo, canales, formato, memoria, pico y RMS, además de las secciones Sound y Settings.

```diagram bars ms
Rehacer 3 LODs de una malla de 8192 triángulos: 37
```
