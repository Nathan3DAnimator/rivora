id: import-report
kind: howto
title: Tlender completo: modificadores, nós, drivers e o Relatório de Importação | The whole Tlender: modifiers, nodes, drivers and the Import Report
keys: relatorio de importacao, import report, informe de importacion, o que se perdeu, what was lost, que se perdio, perdido, lost, perdido na importacao, convertido, converted, assado, baked, horneado, modificador, modifier, modificadores, modifiers, mirror, array, subdivision, subsurf, solidify, weld, displace, decimate, edge split, mask, nonlinear, bevel, boolean, cloth, soft body, malha avaliada, evaluated mesh, nos de shader, shader nodes, nodos, no sem equivalente, node without equivalent, metallic bsdf, glass bsdf, rgb curves, curvas rgb, float curve, light path, object info, blackbody, vector rotate, brick texture, magic texture, stones, pedras, cracks, rachaduras, driver, drivers, expressao, expression, expresion, shape key, corretiva, corrective, curva de morph, morph curve, imagem gerada, generated image, uv grid, udim, textura faltando, missing texture, track to, locked track, limit distance, maintain volume, luz do tlender, tlender light, camera do tlender, tlender camera, importar tlender, import tlender, tlend, vinco, vincos, crease, creases, pliegue, aresta sharp, sharp edge, arista marcada, exportar para a real engine, export to real engine, material assado, baked material
--- pt
```diagram steps
Importar o .tlend (Arquivo > Importar) | A Engine aplica, converte ou assa cada coisa | Janela > Relatório de Importação (abre sozinha se algo se perdeu) | ImportReport.txt ao lado dos assets
```
## O que entra de um .tlend
- **Modificadores** (na ordem da pilha, numa cópia da malha do objeto; grupos de vértices e shape keys acompanham): **Mirror**, **Array**, **Subdivision Surface**, **Solidify**, **Weld**, **Displace**, **Mask**, **Decimate** e o **Nonlinear** do Tlender são aplicados. **Armature** vira o Skeletal Mesh, **Cloth** e **Soft Body** viram o componente **Tecido**, **Edge Split** vira o ângulo das arestas duras, **Triangulate** é o que a Engine já faz. Bevel, Boolean, Remesh, Geometry Nodes e as deformações por outro objeto entram exatos quando o Tlender grava a **malha avaliada** (Exportar para a Real Engine); sem ela, ficam como **Perdido** com o motivo.
- **Exportar do Tlender para a Engine**: no Tlender, use **Arquivo > Exportar > Real Engine (.tlend)**. Esse arquivo leva as malhas avaliadas e os nós assados; o Ctrl+S não leva. Um material próprio do Tlender (Solo, Água, Pedra...) chega como um **Principled** feito dos mapas assados (cor, rugosidade, normal pelo **Normal Map**, emissão ou metálico). Os **vincos** das arestas entram na Subdivision Surface como no Tlender, e as **arestas sharp** dividem as normais.
- **Nós de shader**: os que a Engine tem entram iguais. **Brick**, **Magic**, **Vector Rotate**, **Blackbody** e as texturas do Tlender (**Stones**, **Cracks**, **Grains**, **Weave**, **Fibers**, **Ocean**) são **assados na GPU** nas texturas do material (a mesma matemática do Tlender). Os sem equivalente viram nós da Engine: Metallic, Glass, Sheen, SSS, Hair e Toon BSDF viram um **Principled** (ou o Shading Model Toon), **RGB/Vector/Float Curves** viram **Color Ramps** que seguem a curva, **Light Path** e **Object Info** viram constantes. Shaders de volume e o céu do mundo se perdem (com o motivo).
- **Imagens**: copiadas para `Textures/`; uma imagem **gerada** (Blank, UV Grid, Color Grid) vira PNG; uma que não está onde o arquivo diz é procurada pelo nome perto do .tlend; UDIM vira um `.rudim` com todos os tiles.
- **Drivers**: avaliados como no Tlender a cada quadro das animações. Um driver constante vira o valor no asset; um que muda é **assado** nas poses dos ossos e nas **curvas de morph** (a shape key corretiva segue o osso).
- **Rig**: Track To, Locked Track, Limit Distance e Maintain Volume agora são avaliados também. **Luzes** e **câmeras** do .tlend viram atores.

## O relatório
1. Importe o arquivo. Se algo se perdeu, a janela **Relatório de Importação** abre sozinha (também em **Janela > Relatório de Importação**). [Mostrar onde](show:Window>Import Report)
2. Cada linha diz o **Resultado** (Igual, Convertido, Assado, Perdido), a categoria, o item, o dono (objeto, material, osso) e o **motivo**.
3. Filtre pelas caixas de cada resultado ou pela busca. **Mostrar o Arquivo do Relatório** abre o `ImportReport.txt` gravado ao lado dos assets.

--- en
```diagram steps
Import the .tlend (File > Import) | The Engine applies, converts or bakes each thing | Window > Import Report (opens by itself when something was lost) | ImportReport.txt next to the assets
```
## What comes in from a .tlend
- **Modifiers** (in the stack's order, on a copy of the object's mesh; vertex groups and shape keys follow): **Mirror**, **Array**, **Subdivision Surface**, **Solidify**, **Weld**, **Displace**, **Mask**, **Decimate** and the Tlender's **Nonlinear** are applied. **Armature** becomes the Skeletal Mesh, **Cloth** and **Soft Body** the **Cloth** component, **Edge Split** the hard edges' angle, **Triangulate** is what the Engine does anyway. Bevel, Boolean, Remesh, Geometry Nodes and the deforms by another object come in exactly when the Tlender writes the **evaluated mesh** (Export to Real Engine); without it they are **Lost**, with the reason.
- **Exporting from the Tlender to the Engine**: in the Tlender, use **File > Export > Real Engine (.tlend)**. That file carries the evaluated meshes and the baked nodes; Ctrl+S does not. A material of the Tlender's own (Soil, Water, Rock...) arrives as a **Principled** made of its baked maps (color, roughness, the normal through a **Normal Map**, emission or metallic). The edges' **creases** go into the Subdivision Surface as in the Tlender, and the **sharp edges** split the normals.
- **Shader nodes**: the ones the Engine has come in the same. **Brick**, **Magic**, **Vector Rotate**, **Blackbody** and the Tlender's textures (**Stones**, **Cracks**, **Grains**, **Weave**, **Fibers**, **Ocean**) are **baked on the GPU** into the material's textures (the Tlender's math). The ones without an equivalent become Engine nodes: the Metallic, Glass, Sheen, SSS, Hair and Toon BSDFs a **Principled** (or the Toon shading model), **RGB/Vector/Float Curves** **Color Ramps** that follow the curve, **Light Path** and **Object Info** constants. Volume shaders and the world's sky are lost (with the reason).
- **Images**: copied to `Textures/`; a **generated** image (Blank, UV Grid, Color Grid) becomes a PNG; one that isn't where the file says is found by its name near the .tlend; a UDIM becomes one `.rudim` with all its tiles.
- **Drivers**: evaluated as in the Tlender on every frame of the animations. A constant driver becomes the value in the asset; one that changes is **baked** into the bones' poses and the **morph curves** (a corrective shape key follows its bone).
- **Rig**: Track To, Locked Track, Limit Distance and Maintain Volume are evaluated too. The .tlend's **lights** and **cameras** become actors.

## The report
1. Import the file. If something was lost, the **Import Report** window opens by itself (also in **Window > Import Report**). [Show me where](show:Window>Import Report)
2. Each row says the **Result** (Same, Converted, Baked, Lost), the category, the item, its owner (object, material, bone) and the **reason**.
3. Filter by each result's box or by the search. **Show Report File** opens the `ImportReport.txt` written next to the assets.

--- es
```diagram steps
Importar el .tlend (Archivo > Importar) | La Engine aplica, convierte u hornea cada cosa | Ventana > Informe de Importación (se abre sola si algo se perdió) | ImportReport.txt junto a los assets
```
## Qué entra de un .tlend
- **Modificadores** (en el orden de la pila, sobre una copia de la malla del objeto; los grupos de vértices y las shape keys siguen): **Mirror**, **Array**, **Subdivision Surface**, **Solidify**, **Weld**, **Displace**, **Mask**, **Decimate** y el **Nonlinear** del Tlender se aplican. **Armature** se vuelve el Skeletal Mesh, **Cloth** y **Soft Body** el componente **Tela**, **Edge Split** el ángulo de las aristas duras, **Triangulate** es lo que la Engine ya hace. Bevel, Boolean, Remesh, Geometry Nodes y las deformaciones por otro objeto entran exactos cuando el Tlender graba la **malla evaluada** (Exportar a Real Engine); sin ella quedan como **Perdido**, con el motivo.
- **Nodos de shader**: los que la Engine tiene entran iguales. **Brick**, **Magic**, **Vector Rotate**, **Blackbody** y las texturas del Tlender (**Stones**, **Cracks**, **Grains**, **Weave**, **Fibers**, **Ocean**) se **hornean en la GPU** en las texturas del material (la misma matemática del Tlender). Los sin equivalente se vuelven nodos de la Engine: los BSDF Metallic, Glass, Sheen, SSS, Hair y Toon un **Principled** (o el Shading Model Toon), las **RGB/Vector/Float Curves** **Color Ramps** que siguen la curva, **Light Path** y **Object Info** constantes. Los shaders de volumen y el cielo del mundo se pierden (con el motivo).
- **Imágenes**: copiadas a `Textures/`; una imagen **generada** (Blank, UV Grid, Color Grid) se vuelve PNG; una que no está donde el archivo dice se busca por su nombre cerca del .tlend; un UDIM se vuelve un `.rudim` con todos sus tiles.
- **Drivers**: evaluados como en el Tlender en cada cuadro de las animaciones. Un driver constante se vuelve el valor en el asset; uno que cambia se **hornea** en las poses de los huesos y en las **curvas de morph** (la shape key correctiva sigue a su hueso).
- **Rig**: Track To, Locked Track, Limit Distance y Maintain Volume ahora también se evalúan. Las **luces** y **cámaras** del .tlend se vuelven actores.

## El informe
1. Importa el archivo. Si algo se perdió, la ventana **Informe de Importación** se abre sola (también en **Ventana > Informe de Importación**). [Mostrar dónde](show:Window>Import Report)
2. Cada fila dice el **Resultado** (Igual, Convertido, Horneado, Perdido), la categoría, el ítem, su dueño (objeto, material, hueso) y el **motivo**.
3. Filtra por la casilla de cada resultado o por la búsqueda. **Mostrar el archivo del informe** abre el `ImportReport.txt` grabado junto a los assets.
