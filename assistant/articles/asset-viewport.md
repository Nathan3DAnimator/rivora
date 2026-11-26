id: asset-viewport
kind: howto
title: O viewport dos editores de asset | The asset editors' viewport | La ventana gráfica de los editores de assets
keys: viewport do asset, asset viewport, prévia 3d, preview 3d, visualizador 3d, 3d viewer, modo de visualização, view mode, iluminado, lit, unlit, sem iluminação, wireframe, albedo, textura, texture, normais, normals, rugosidade, roughness, metálico, metallic, oclusão ambiente, ambient occlusion, ao, uv, layout de uv, uv layout, lod, cores por lod, lod coloration, overdraw, sobreposição, ossos, bones, mostrar ossos, show bones, nomes dos ossos, bone names, eixos, axes, socket, sockets, adicionar socket, add socket, malha de prévia, preview mesh, corpos de física, physics bodies, juntas, constraints, céu, sky, hdri, estúdio, studio, luz, light, girar a luz, rotate light, câmera, camera, voar, fly, órbita, orbit, enquadrar, frame, campo de visão, field of view, fov, abas, tabs, modelo, model, esqueleto, skeleton, animação, animation, física, physics, ik rig, ragdoll, physics asset, skeletal mesh, static mesh, material, partículas, particles, veículo, vehicle, tecido, cloth, modo de vista, ver el modelo, huesos, cielo, luz, pestañas
--- pt
```diagram steps
Abra o asset (clique duplo no Navegador de Conteúdo) | O modelo aparece no viewport, com materiais e sombra | Escolha o modo (Iluminado, Albedo, AO, UV, LOD...) | Mostrar: ossos, sockets, corpos | Abas no canto: Modelo, Esqueleto, Animação, Física
```
## Um viewport em todo editor
Todo editor com 3D usa o mesmo viewport: Static Mesh, Skeletal Mesh, Esqueleto, Animação, Montage, Blend Space, Anim Graph, Physics Asset, IK Rig, Material, Partículas, Veículo e Tecido. **O modelo sempre aparece**, desenhado pela GPU com os materiais e as texturas, a sombra e o chão.

## A barra de cima
- **Modo de visualização**: Iluminado, Sem Iluminação, Wireframe, Iluminado com Wireframe, Só Iluminação, **Albedo** (só a cor base), Normal do Mundo, Rugosidade, Metálico, **Oclusão Ambiente**, **UV** (xadrez pelas coordenadas e o layout de UV no canto), **Cores por LOD** (branco 0, vermelho 1, verde 2, azul 3) e **Overdraw** (quanto mais camadas, mais amarelo).
- **Mostrar**: ossos (nenhum, só os selecionados ou todos), os nomes e os eixos deles, os sockets, os corpos de física e as juntas, o chão, a grade, o layout de UV e as estatísticas (triângulos, LOD, ms).
- **Iluminação**: Estúdio ou Céu Aberto; a direção, a altura e a força da luz principal. **L + arrastar** na vista gira a luz.
- **Câmera**: campo de visão, velocidade de voo, Enquadrar (F).
- **LOD**: Automático (o que o jogo desenharia nesta distância) ou um fixo, com os triângulos de cada um.

## Mexer a câmera
- Arrastar com o **esquerdo**: orbitar. **Meio**: mover. **Roda**: aproximar.
- **Direito + W A S D** (Q e E descem e sobem): voar; a roda durante o voo muda a velocidade.
- **Alt + direito**: aproximar arrastando. **F**: enquadrar o modelo.

## Trocar de modo sem fechar a janela
No canto superior direito do Skeletal Mesh, da Animação, do Physics Asset e do IK Rig ficam as abas **Modelo, Esqueleto, Animação, Física** (e **IK Rig** quando a malha tem um). A mesma janela troca o que edita: estando na animação, clique em Modelo ou Esqueleto e volte depois. **Física** abre o Physics Asset da malha; se ela não tiver, ele é criado a partir dos ossos.

## Sockets
Na aba **Esqueleto**: selecione um osso e clique em **Adicionar Socket** (ou clique direito no osso da árvore). Dê um nome, ajuste a posição e a rotação, ou **arraste as setas do gizmo** no viewport. **Malha de Prévia** mostra uma malha no socket (uma arma, um chapéu) só nos editores, para conferir o lugar. Salve com Ctrl+S.
--- en
```diagram steps
Open the asset (double-click in the Content Browser) | The model shows in the viewport, with its materials and shadow | Pick the mode (Lit, Albedo, AO, UV, LOD...) | Show: bones, sockets, bodies | Tabs in the corner: Model, Skeleton, Animation, Physics
```
## One viewport in every editor
Every editor with a 3D view uses the same viewport: Static Mesh, Skeletal Mesh, Skeleton, Animation, Montage, Blend Space, Anim Graph, Physics Asset, IK Rig, Material, Particles, Vehicle and Cloth. **The model always shows**, drawn by the GPU with its materials and textures, its shadow and the floor.

## The top bar
- **View mode**: Lit, Unlit, Wireframe, Lit Wireframe, Lighting Only, **Albedo** (the base color alone), World Normal, Roughness, Metallic, **Ambient Occlusion**, **UV** (a checker by the coordinates and the UV layout in the corner), **LOD Coloration** (white 0, red 1, green 2, blue 3) and **Overdraw** (the more layers, the yellower).
- **Show**: bones (none, the selected or all), their names and axes, the sockets, the physics bodies and constraints, the floor, the grid, the UV layout and the statistics (triangles, LOD, ms).
- **Lighting**: Studio or Outdoor Sky; the key light's direction, height and strength. **L + drag** in the view turns the light.
- **Camera**: field of view, flying speed, Frame (F).
- **LOD**: Auto (what the game would draw at this distance) or a fixed one, with each one's triangles.

## Moving the camera
- **Left** drag: orbit. **Middle**: pan. **Wheel**: zoom.
- **Right + W A S D** (Q and E go down and up): fly; the wheel while flying changes the speed.
- **Alt + right**: dolly. **F**: frame the model.

## Switching without closing the window
At the top right of the Skeletal Mesh, Animation, Physics Asset and IK Rig editors are the tabs **Model, Skeleton, Animation, Physics** (and **IK Rig** when the mesh has one). The same window switches what it edits: in an animation, click Model or Skeleton and come back later. **Physics** opens the mesh's Physics Asset; when it has none, one is made from its bones.

## Sockets
In the **Skeleton** tab: select a bone and click **Add Socket** (or right-click the bone in the tree). Name it, set its location and rotation, or **drag the gizmo's arrows** in the viewport. **Preview Mesh** shows a mesh on the socket (a weapon, a hat) in the editors only, to check the place. Save with Ctrl+S.
--- es
```diagram steps
Abre el asset (doble clic en el Explorador de contenidos) | El modelo aparece en la ventana gráfica, con sus materiales y su sombra | Elige el modo (Lit, Albedo, AO, UV, LOD...) | Show: huesos, sockets, cuerpos | Pestañas en la esquina: Model, Skeleton, Animation, Physics
```
## Una ventana gráfica en cada editor
Todos los editores con vista 3D usan la misma ventana gráfica: Static Mesh, Skeletal Mesh, Skeleton, Animation, Montage, Blend Space, Anim Graph, Physics Asset, IK Rig, Material, Particles, Vehicle y Cloth. **El modelo siempre se muestra**, dibujado por la GPU con sus materiales y texturas, su sombra y el suelo.

## La barra superior
- **Modo de vista**: Lit, Unlit, Wireframe, Lit Wireframe, Lighting Only, **Albedo** (solo el color base), World Normal, Roughness, Metallic, **Ambient Occlusion**, **UV** (un damero según las coordenadas y el diseño UV en la esquina), **LOD Coloration** (blanco 0, rojo 1, verde 2, azul 3) y **Overdraw** (cuantas más capas, más amarillo).
- **Show**: huesos (ninguno, el seleccionado o todos), sus nombres y ejes, los sockets, los cuerpos y restricciones de física, el suelo, la cuadrícula, el diseño UV y las estadísticas (triángulos, LOD, ms).
- **Lighting**: Studio o Outdoor Sky; la dirección, la altura y la intensidad de la luz principal. **L + arrastrar** en la vista gira la luz.
- **Cámara**: campo de visión, velocidad de vuelo, Frame (F).
- **LOD**: Auto (lo que el juego dibujaría a esta distancia) o uno fijo, con los triángulos de cada uno.

## Mover la cámara
- Arrastrar con el botón **izquierdo**: orbitar. **Central**: desplazar. **Rueda**: zoom.
- **Derecho + W A S D** (Q y E bajan y suben): volar; la rueda mientras vuelas cambia la velocidad.
- **Alt + derecho**: dolly. **F**: encuadrar el modelo.

## Cambiar sin cerrar la ventana
En la parte superior derecha de los editores de Skeletal Mesh, Animation, Physics Asset e IK Rig están las pestañas **Model, Skeleton, Animation, Physics** (e **IK Rig** cuando la malla tiene uno). La misma ventana cambia lo que edita: en una animación, haz clic en Model o Skeleton y vuelve más tarde. **Physics** abre el Physics Asset de la malla; si no tiene ninguno, se crea uno a partir de sus huesos.

## Sockets
En la pestaña **Skeleton**: selecciona un hueso y haz clic en **Add Socket** (o clic derecho en el hueso del árbol). Ponle un nombre, define su ubicación y rotación, o **arrastra las flechas del gizmo** en la ventana gráfica. **Preview Mesh** muestra una malla en el socket (un arma, un sombrero) solo en los editores, para comprobar el lugar. Guarda con Ctrl+S.
