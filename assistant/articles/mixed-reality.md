id: mixed-reality
kind: howto
title: Realidade mista no Quest: passthrough, o quarto real, oclusão | Mixed reality on the Quest: passthrough, the real room, occlusion | Realidad mixta en el Quest: passthrough, la habitación real, oclusión
keys: realidade mista, mixed reality, realidad mixta, mr, passthrough, passagem, ver o quarto, see the room, ver la habitacion, camera do quest, cameras do headset, headset cameras, quest 3, quest 3s, quest pro, ar no quest, xr, space setup, configuracao de espaco, configuração de espaço, scene, cena do quarto, room scan, escanear o quarto, escanear la habitacion, planos do quarto, room planes, chao real, chão real, real floor, suelo real, parede real, real wall, mesa real, real table, sofa real, real couch, malha do quarto, room mesh, global mesh, oclusao, oclusão, occlusion, oclusion, profundidade real, real depth, environment depth, profundidad real, depth api, sombra no chao real, sombra no chão real, shadow on the real floor, janela para o mundo real, passthrough window, portal, opacidade do passthrough, passthrough opacity, estilo do passthrough, passthrough style, preto e branco, black and white, visao noturna, visão noturna, night vision, set passthrough enabled, line trace real world, get room planes, event on room loaded, request room setup, developer runtime features, spatial data over meta quest link, use_scene
--- pt
```diagram steps
Project Settings > VR > Realidade Mista (Passthrough) ligada | Play > Prévia em VR (Link: Developer Runtime Features ligadas) ou o headset simulado | O quarto do Space Setup vira atores MR Floor, MR Wall, MR Table que colidem | O virtual sobre o quarto real: oclusão, sombras, janelas, estilos
```
## Ligar
- **Project Settings > VR > Realidade Mista (Passthrough)**: o jogo pede ao headset o passthrough (o quarto pelas câmeras), o quarto do **Space Setup** e a **profundidade real**. Quest 3, 3S e Pro (o Quest 2 também, em preto e branco). Desligado: VR puro.
- **Fundo do Passthrough > O Quarto**: o céu do nível some; tudo o que não é desenhado vira o mundo real. **O Céu do Nível**: o quarto só aparece pelas **Passthrough Windows**.
- **No PC pelo Link**: no app Meta Horizon Link, **Configurações > Beta > Developer Runtime Features**, depois **Passthrough over Meta Quest Link** e **Spatial Data over Meta Quest Link**. **No Quest sozinho**: Compilar > Empacotar para Meta Quest já põe a permissão do quarto (USE_SCENE, o headset pergunta uma vez).
- **Sem headset**: o headset simulado (Play > Prévia em VR sem headset, `--vr-sim`) desenha um quarto de mentira de 5 x 4 m com mesa, sofá, janela e porta, com o estilo do passthrough: dá para testar tudo.

## O quarto real (reconstrução do ambiente)
- **Carregar o Quarto**: o Space Setup do headset vira atores **MR Floor**, **MR Ceiling**, **MR Wall 2**, **MR Table**, **MR Couch**... que **colidem e nunca são desenhados**: os Characters andam no chão real, uma bola com física quica na mesa real, o Line Trace by Channel acerta a parede. Sem quarto escaneado, **Pedir o Mapeamento do Quarto** abre o Space Setup.
- **Alinhar o Nível ao Quarto**: o nível gira para o X correr ao longo da parede mais longa: construa o nível com as paredes nos eixos.
- **Malha do Quarto**: a malha escaneada (toda coisa real, uma cadeira, uma caixa): o **Line Trace Real World** a alcança.
- **Get Room Planes** dá os atores; **Get Room Plane Info** diz o rótulo (Floor, Wall, Table, Couch...), o centro, a normal e o tamanho de cada um. **Event On Room Loaded** roda em todo Grafo quando o quarto chega.

## O virtual misturado ao real
- **Oclusão pelo Mundo Real > Profundidade Real**: uma mão ou uma cadeira real na frente esconde o virtual (Quest 3 / 3S), com uma borda suave (**Suavidade da Oclusão**); **Sem as Mãos** deixa as mãos virtuais por cima. Sem profundidade real (o Link), os **planos e móveis do quarto** escondem o que está atrás deles.
- **Sombras no Quarto**: a sombra do sol dos objetos virtuais cai no chão, na mesa e nas paredes reais. **Iluminação > Luz de um Quarto**: o sol vira uma luz quente de cima; **Exposição**: Automática mede só o virtual; um número a fixa (uns 7 EV com a Luz de um Quarto).
- **Passthrough Window**: os limites de um ator viram uma janela para o mundo real (um portal numa parede virtual); **Janelas e Portas Reais** abrem as do quarto.
- **Estilo**: Cor, Preto e Branco, Uma Cor (visão noturna, holograma), Ajustado (brilho, contraste, saturação); **Opacidade**, **Cor das Bordas**, **Opacidade do Virtual** (uma transição entre o VR e o quarto). Tudo também como console variable: `vr.Passthrough`, `vr.PassthroughStyle`, `vr.MROcclusion`, `vr.MRShadows`...

```blueprint
Event BeginPlay -> Set Passthrough Enabled ( true ) -> Set Passthrough Background ( true ) -> Set Real World Occlusion ( 2 )
Event On Room Loaded ( Count ) -> Print String ( "Quarto carregado" )
Event BeginPlay -> Set Passthrough Opacity ( 0.6 ) -> Set Passthrough Blend ( 1 )
```
--- en
```diagram steps
Project Settings > VR > Mixed Reality (Passthrough) on | Play > VR Preview (Link: Developer Runtime Features on) or the simulated headset | The Space Setup's room becomes MR Floor, MR Wall, MR Table actors that collide | The virtual over the real room: occlusion, shadows, windows, styles
```
## Turning it on
- **Project Settings > VR > Mixed Reality (Passthrough)**: the game asks the headset for the passthrough (the room through its cameras), the room of its **Space Setup** and the **real depth**. Quest 3, 3S and Pro (the Quest 2 too, in black and white). Off: pure VR.
- **Passthrough Background > The Room**: the level's sky goes; everything not drawn is the real world. **The Level's Sky**: the room only through **Passthrough Windows**.
- **On a PC by Link**: in the Meta Horizon Link app, **Settings > Beta > Developer Runtime Features**, then **Passthrough over Meta Quest Link** and **Spatial Data over Meta Quest Link**. **On the Quest by itself**: Build > Package for Meta Quest adds the room's permission (USE_SCENE: the headset asks once).
- **Without a headset**: the simulated headset (Play > VR Preview with none, `--vr-sim`) draws a stand-in room of 5 x 4 m with a table, a couch, a window and a door, in the passthrough's style: everything testable.

## The real room (the environment's reconstruction)
- **Load the Room**: the headset's Space Setup becomes **MR Floor**, **MR Ceiling**, **MR Wall 2**, **MR Table**, **MR Couch**... actors that **collide and are never drawn**: the Characters walk on the real floor, a ball with physics bounces on the real table, Line Trace by Channel hits the wall. No room scanned: **Ask for the Room Setup** opens the Space Setup.
- **Align the Level to the Room**: the level turns so its X runs along the longest wall: build the level with its walls on the axes.
- **Room Mesh**: the scanned mesh (every real thing, a chair, a box): **Line Trace Real World** reaches it.
- **Get Room Planes** gives the actors; **Get Room Plane Info** tells each one's label (Floor, Wall, Table, Couch...), center, normal and size. **Event On Room Loaded** runs in every Graph when the room is in.

## The virtual mixed with the real
- **Real World Occlusion > Real Depth**: a real hand or chair in front hides the virtual (Quest 3 / 3S), with a soft edge (**Occlusion Softness**); **Hand Removal** keeps the virtual hands over the real ones. Without a real depth (Link), the **room's planes and furniture** hide what is behind them.
- **Shadows on the Room**: the virtual things' sun shadow falls on the real floor, table and walls. **Lighting > A Room's Light**: the sun becomes a warm light from above; **Exposure**: Automatic meters only the virtual; a number fixes it (about EV 7 with A Room's Light).
- **Passthrough Window**: an actor's bounds become a window to the real world (a portal in a virtual wall); **Real Windows and Doors** opens the room's own.
- **Style**: Color, Black & White, One Color (night vision, a hologram), Adjusted (brightness, contrast, saturation); **Opacity**, **Edge Color**, **Virtual Opacity** (a fade between VR and the room). All of it as console variables too: `vr.Passthrough`, `vr.PassthroughStyle`, `vr.MROcclusion`, `vr.MRShadows`...

```blueprint
Event BeginPlay -> Set Passthrough Enabled ( true ) -> Set Passthrough Background ( true ) -> Set Real World Occlusion ( 2 )
Event On Room Loaded ( Count ) -> Print String ( "Room loaded" )
Event BeginPlay -> Set Passthrough Opacity ( 0.6 ) -> Set Passthrough Blend ( 1 )
```
--- es
```diagram steps
Project Settings > VR > Mixed Reality (Passthrough) activado | Jugar > VR Preview (Link: Developer Runtime Features activado) o el visor simulado | La habitación del Space Setup se convierte en actores MR Floor, MR Wall, MR Table que colisionan | Lo virtual sobre la habitación real: oclusión, sombras, ventanas, estilos
```
## Activarlo
- **Project Settings > VR > Mixed Reality (Passthrough)**: el juego le pide al visor el passthrough (la habitación a través de sus cámaras), la habitación de su **Space Setup** y la **profundidad real**. Quest 3, 3S y Pro (el Quest 2 también, en blanco y negro). Desactivado: VR pura.
- **Passthrough Background > The Room**: el cielo del nivel desaparece; todo lo que no se dibuja es el mundo real. **The Level's Sky**: la habitación solo a través de **Passthrough Windows**.
- **En un PC por Link**: en la app Meta Horizon Link, **Settings > Beta > Developer Runtime Features**, luego **Passthrough over Meta Quest Link** y **Spatial Data over Meta Quest Link**. **En el Quest solo**: Compilar > Empaquetar para Meta Quest añade el permiso de la habitación (USE_SCENE: el visor lo pide una vez).
- **Sin visor**: el visor simulado (Jugar > VR Preview sin ninguno, `--vr-sim`) dibuja una habitación de reemplazo de 5 x 4 m con una mesa, un sofá, una ventana y una puerta, al estilo del passthrough: todo se puede probar.

## La habitación real (la reconstrucción del entorno)
- **Load the Room**: el Space Setup del visor se convierte en actores **MR Floor**, **MR Ceiling**, **MR Wall 2**, **MR Table**, **MR Couch**... que **colisionan y nunca se dibujan**: los Characters caminan sobre el suelo real, una pelota con física rebota en la mesa real, Line Trace by Channel golpea la pared. Sin habitación escaneada: **Ask for the Room Setup** abre el Space Setup.
- **Align the Level to the Room**: el nivel gira para que su X corra a lo largo de la pared más larga: construye el nivel con sus paredes sobre los ejes.
- **Room Mesh**: la malla escaneada (todo lo real, una silla, una caja): **Line Trace Real World** llega hasta ella.
- **Get Room Planes** da los actores; **Get Room Plane Info** dice la etiqueta de cada uno (Floor, Wall, Table, Couch...), centro, normal y tamaño. **Event On Room Loaded** se ejecuta en todo Grafo cuando la habitación está cargada.

## Lo virtual mezclado con lo real
- **Real World Occlusion > Real Depth**: una mano o una silla reales delante ocultan lo virtual (Quest 3 / 3S), con un borde suave (**Occlusion Softness**); **Hand Removal** mantiene las manos virtuales sobre las reales. Sin profundidad real (Link), los **planos y muebles de la habitación** ocultan lo que hay detrás.
- **Shadows on the Room**: la sombra del sol de las cosas virtuales cae sobre el suelo, la mesa y las paredes reales. **Lighting > A Room's Light**: el sol se convierte en una luz cálida desde arriba; **Exposure**: Automatic mide solo lo virtual; un número lo fija (cerca de EV 7 con A Room's Light).
- **Passthrough Window**: los límites de un actor se convierten en una ventana al mundo real (un portal en una pared virtual); **Real Windows and Doors** abre las de la propia habitación.
- **Style**: Color, Black & White, One Color (visión nocturna, un holograma), Adjusted (brillo, contraste, saturación); **Opacity**, **Edge Color**, **Virtual Opacity** (un fundido entre la VR y la habitación). Todo también como variables de consola: `vr.Passthrough`, `vr.PassthroughStyle`, `vr.MROcclusion`, `vr.MRShadows`...

```blueprint
Event BeginPlay -> Set Passthrough Enabled ( true ) -> Set Passthrough Background ( true ) -> Set Real World Occlusion ( 2 )
Event On Room Loaded ( Count ) -> Print String ( "Room loaded" )
Event BeginPlay -> Set Passthrough Opacity ( 0.6 ) -> Set Passthrough Blend ( 1 )
```
