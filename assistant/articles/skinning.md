id: skinning
kind: concept
title: Skinning suave: roupas e corpos que dobram | Smooth skinning: clothes and bodies that bend | Skinning: personajes que se doblan sin costuras
keys: skinning, skin, pele, pesos, weights, vertex groups, grupos de vertices, vestido cortado, roupa cortada, roupa rasgada, costura, emenda, seam, dress, cloth, personagem cortado, skin cache, deformar, deform, skeletal mesh, reimportar, reimport, camadas de roupa, roupa atravessando, visualizador vazio, skeletal mesh vazio, preview vazio, nao aparece no editor, empty viewer
--- pt
```diagram steps
Tlender / glTF / FBX (pesos por vértice) | Importar: uma pele por material (4 ossos por vértice) | Animação: paleta dos ossos | GPU dobra os vértices a cada pose | Sombras e GI por raio na pele dobrada
```
## O que é
Um personagem importado com **armature** vira um **Skeletal Mesh** com **peles**: cada vértice segue até **4 ossos**, com os pesos que você pintou no Tlender (ou que vêm no glTF e no FBX). A GPU dobra os vértices a cada pose (como o *Skin Cache* da Unreal), então vestido, avental e mangas se movem inteiros, sem emendas nem uma camada de roupa atravessando a outra.
- Só roda quando a pose muda: um personagem parado não custa nada.
- As sombras por raio e a GI veem a pele dobrada (o BLAS dela é atualizado junto).
- Malhas presas a um osso sem pesos (chapéu, arma) entram na pele com peso 1 nesse osso.

## Personagem importado antes desta versão
Os personagens antigos eram **cortados em partes rígidas por osso**: dava roupa "cortada" e emendas que piscavam. O editor avisa no Details do ator. Clique com o botão direito no Skeletal Mesh no Content Browser > **Reimportar** (ou o botão **Reimportar** no Details): ele lê o arquivo de origem de novo para a mesma pasta e os atores já colocados ganham as peles na hora. Nos personagens importados antes de existir o Reimportar, o editor pergunta onde está o arquivo (uma vez; depois ele lembra).

## Desempenho
Medido com a Delight (5 peles, 57 mil vértices) dançando, em 1440p: **495 fps**, 0,004 ms de CPU. A linha `Skins:` do `--bench` mostra quantas peles foram dobradas no último quadro.
- **Editores:** o Skeletal Mesh, as animações, o Blend Space e o Anim Graph mostram a pele deformada pela pose (antes só as partes antigas apareciam e a Delight ficava vazia).
--- en
```diagram steps
Tlender / glTF / FBX (per-vertex weights) | Import: one skin per material (4 bones per vertex) | Animation: the bones' palette | The GPU bends the vertices each pose | Ray traced shadows and GI on the bent skin
```
## What it is
A character imported with an **armature** becomes a **Skeletal Mesh** with **skins**: each vertex follows up to **4 bones** with the weights you painted in the Tlender (or that come in glTF and FBX). The GPU bends the vertices every pose (like Unreal's *Skin Cache*), so a dress, an apron and sleeves move whole, with no seams and no layer of cloth crossing another.
- It only runs when the pose changes: a still character costs nothing.
- Ray traced shadows and GI see the bent skin (its BLAS is updated with it).
- Meshes held by a bone without weights (a hat, a weapon) go into the skin with weight 1 on that bone.

## A character imported before this version
Older characters were **cut into rigid parts by bone**: clothes looked "cut" and seams flickered. The editor warns in the actor's Details. Right click the Skeletal Mesh in the Content Browser > **Reimport** (or the **Reimport** button in Details): it reads the source file again into the same folder and the actors already placed get the skins at once. For characters imported before Reimport existed, the editor asks where the file is (once; then it remembers).

## Performance
Measured with the Delight (5 skins, 57 thousand vertices) dancing at 1440p: **495 fps**, 0.004 ms of CPU. The `Skins:` line of `--bench` shows how many skins were bent in the last frame.
- **Editors:** the Skeletal Mesh, the animations, the Blend Space and the Anim Graph show the skin bent by the pose (before, only the old parts showed and the Delight was empty).
--- es
```diagram steps
Tlender / glTF / FBX (pesos por vértice) | Importación: un skin por material (4 huesos por vértice) | Animación: la paleta de huesos | La GPU dobla los vértices en cada pose | Sombras y GI por trazado de rayos sobre el skin doblado
```
## Qué es
Un personaje importado con una **armature** se convierte en un **Skeletal Mesh** con **skins**: cada vértice sigue hasta **4 huesos** con los pesos que pintaste en el Tlender (o que vienen en glTF y FBX). La GPU dobla los vértices en cada pose (como el *Skin Cache* de Unreal), así que un vestido, un delantal y unas mangas se mueven enteros, sin costuras y sin que una capa de tela atraviese a otra.
- Solo se ejecuta cuando cambia la pose: un personaje quieto no cuesta nada.
- Las sombras y la GI por trazado de rayos ven el skin doblado (su BLAS se actualiza con él).
- Las mallas sujetas a un hueso sin pesos (un sombrero, un arma) entran en el skin con peso 1 en ese hueso.

## Un personaje importado antes de esta versión
Los personajes antiguos estaban **cortados en piezas rígidas por hueso**: la ropa parecía "cortada" y las costuras parpadeaban. El editor avisa en los Detalles del actor. Clic derecho en el Skeletal Mesh en el Explorador de contenidos > **Reimport** (o el botón **Reimport** en Detalles): vuelve a leer el archivo de origen en la misma carpeta y los actores ya colocados reciben los skins al instante. Para los personajes importados antes de que existiera Reimport, el editor pregunta dónde está el archivo (una vez; después lo recuerda).

## Rendimiento
Medido con la Delight (5 skins, 57 mil vértices) bailando a 1440p: **495 fps**, 0,004 ms de CPU. La línea `Skins:` de `--bench` muestra cuántos skins se doblaron en el último fotograma.
- **Editores:** el Skeletal Mesh, las animaciones, el Blend Space y el Anim Graph muestran el skin doblado por la pose (antes solo se veían las piezas antiguas y la Delight aparecía vacía).
