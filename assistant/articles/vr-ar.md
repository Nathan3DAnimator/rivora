id: vr-ar
kind: howto
title: VR e AR: headset, controles e luz que combina com a câmera | VR and AR: headset, controllers and light matching the camera | VR y AR: visor, controles y luz que combina con la cámara
keys: vr, realidade virtual, virtual reality, ar, realidade aumentada, augmented reality, headset, oculus, quest, meta, steamvr, openxr, controle de vr, motion controller, hmd, oculos, giroscopio, gyroscope, light estimation, estimar luz, color matching, balanco de branco, white balance, haptico, haptic, vibrar controle, arcore, rastreamento, tracking, 6dof, planos, planes, chao real, real floor, shadow catcher, oclusao, occlusion, profundidade, depth, depth api, objeto real na frente, esconder atras do sofa, sombra no chao real, camera do celular, phone camera, ar line trace, tocar na tela para colocar, tap to place, event touch, toque na tela, touch screen, get mouse position, template de ar, ar template
--- pt
```diagram steps
Empacote o jogo | Conecte o headset (Quest Link, SteamVR...) | Jogo.exe --vr (sem headset: --vr-sim mostra os dois olhos) | Blueprint: Get HMD Pose, Get Motion Controller Pose/Input, Play Haptic Effect
```
## VR
- A engine fala **OpenXR** direto com o runtime instalado (Meta, SteamVR, WMR): não precisa de SDK.
- Os dois olhos saem num quadro só do motor, com **MSAA 4×** (o que tem histórico de tela fica desligado no VR para não misturar os olhos); o primeiro olho paga as simulações, a GI e as sombras, o segundo só a vista dele. Meta: **acima de 100 fps**. Teleporte, mãos, pegar e o Quest sozinho: o artigo **Jogos em VR** (`vr-quest`).
- Controles: **Get Motion Controller Pose** (lugar e direção), **Get Motion Controller Input** (gatilho, grip, analógico, botões), **Play Haptic Effect** (vibração), **Get HMD Pose**, **Is VR Active**.
```blueprint
Event Tick -> Get Motion Controller Input ( 1 ) -> Branch ( Trigger > 0.5 ) -> Play Haptic Effect ( 1, 0.6, 0.1 )
```
## AR
- **Luz e cor iguais às da câmera**: a engine mede a exposição, o balanço de branco (temperatura de cor), a luz principal (de onde vem e quão forte) e a luz ambiente da imagem, e aplica no sol, no céu e na exposição; os objetos virtuais ficam com a luz real.
- No celular, o **giroscópio** gira a câmera (`--ar`). No PC dá para testar com uma foto: `Jogo.exe --ar-image foto.png`.

## AR com ARCore (Android)
```diagram steps
Project Settings > Android > Realidade Aumentada (ARCore) | Compilar > Empacotar para Android | No celular: permitir a câmera, mover devagar sobre uma superfície | O jogo aparece no mundo real: rastreado, sobre o chão de verdade, com a sombra nele
```
- O celular é **rastreado em 6 graus de liberdade**: a câmera do jogo é a do celular, andando pelo quarto.
- A **imagem da câmera** fica atrás do nível; os objetos virtuais ficam na frente com a luz do lugar (direção, cor e intensidade estimadas pela ARCore).
- Os **planos** que a ARCore acha (chão, mesas, paredes) viram atores **AR Plane** que bloqueiam: os personagens andam no chão de verdade. O chão real **recebe as sombras** dos objetos virtuais (e o escurecido de contato embaixo deles).
- **Oclusão**: uma coisa real mais perto que uma virtual a esconde (o sofá na frente do cachorro virtual), pela profundidade da ARCore nos celulares que a têm. Project Settings > Android > **Oclusão pelos Objetos Reais** (ligada por padrão). No PC, `REALENGINE_AR_SIM_FLOOR=60` sobe o chão simulado 60 cm: a base dos objetos some atrás dele.
- Para começar pronto: **Novo Projeto > Realidade Aumentada** (a `BP_ARPlacer` põe uma caixa onde você toca no chão real; o AR já vem ligado).
- O toque: **Event Touch** (Pressed / Released, Location em pixels, Finger Index; no PC o botão esquerdo do mouse é um dedo) e **Get Mouse Position**. Os dedos que um widget de toque pegou (joystick, botão) não disparam o evento.
- Nós: **Is AR Tracking**, **AR Line Trace** (um ponto da tela no mundo real: colocar algo onde o jogador toca) e **Get AR Plane Count**.
- O AR é **opcional**: celulares sem o Google Play Services para AR rodam o jogo normalmente. No PC: `Jogo.exe --ar-image foto.png --ar-sim` simula (a foto atrás, um chão na origem).
```blueprint
Event Touch (Pressed) -> Break Vector ( Location ) -> AR Line Trace ( X, Y ) -> Branch ( Hit ) -> Spawn Actor from Class ( BP_Cadeira, Location )
```
--- en
```diagram steps
Package the game | Connect the headset (Quest Link, SteamVR...) | Game.exe --vr (no headset: --vr-sim shows both eyes) | Blueprint: Get HMD Pose, Get Motion Controller Pose/Input, Play Haptic Effect
```
## VR
- The engine talks **OpenXR** directly to the installed runtime (Meta, SteamVR, WMR): no SDK needed.
- Both eyes come out of one frame of the engine with **MSAA 4x** (what keeps a screen history is off in VR so the eyes don't mix); the first eye pays the simulations, the GI and the shadows, the second only its view. Target: **above 100 fps**. Teleport, hands, grabbing and the Quest standalone: the **VR games** article (`vr-quest`).
- Controllers: **Get Motion Controller Pose** (place and direction), **Get Motion Controller Input** (trigger, grip, thumbstick, buttons), **Play Haptic Effect** (vibration), **Get HMD Pose**, **Is VR Active**.
```blueprint
Event Tick -> Get Motion Controller Input ( 1 ) -> Branch ( Trigger > 0.5 ) -> Play Haptic Effect ( 1, 0.6, 0.1 )
```
## AR
- **Light and color like the camera's**: the engine measures the picture's exposure, white balance (color temperature), main light (where from and how strong) and ambient light, and applies them to the sun, the sky and the exposure; virtual objects get the real light.
- On a phone the **gyroscope** turns the camera (`--ar`). On a PC test it with a photo: `Game.exe --ar-image photo.png`.

## AR with ARCore (Android)
```diagram steps
Project Settings > Android > Augmented Reality (ARCore) | Build > Package for Android | On the phone: allow the camera, move slowly over a surface | The game stands in the real world: tracked, on the real floor, casting its shadow on it
```
- The phone is **tracked with 6 degrees of freedom**: the game's camera is the phone's, walking around the room.
- The **camera's picture** is behind the level; the virtual things stand in front of it with the place's light (direction, color and strength estimated by ARCore).
- The **planes** ARCore finds (floors, tables, walls) become blocking **AR Plane** actors: the Characters walk on the real floor. The real floor **receives the shadows** of the virtual things (and the contact darkening under them).
- **Occlusion**: a real thing nearer than a virtual one hides it (the sofa in front of the virtual dog), from ARCore's depth on the phones that have it. Project Settings > Android > **Occlusion by Real Objects** (on by default). On a PC, `REALENGINE_AR_SIM_FLOOR=60` raises the simulated floor 60 cm: the things' bases hide behind it.
- Ready to start: **New Project > Augmented Reality** (`BP_ARPlacer` places a box where you touch the real floor; AR already on).
- The touch: **Event Touch** (Pressed / Released, Location in pixels, Finger Index; on a PC the left mouse button is a finger) and **Get Mouse Position**. The fingers a touch widget took (joystick, button) don't fire it.
- Nodes: **Is AR Tracking**, **AR Line Trace** (a point of the screen onto the real world: place something where the player taps) and **Get AR Plane Count**.
- AR is **optional**: phones without Google Play Services for AR run the game as usual. On a PC: `Game.exe --ar-image photo.png --ar-sim` simulates it (the photo behind, a floor at the origin).
```blueprint
Event Touch (Pressed) -> Break Vector ( Location ) -> AR Line Trace ( X, Y ) -> Branch ( Hit ) -> Spawn Actor from Class ( BP_Chair, Location )
```
--- es
```diagram steps
Empaquetar el juego | Conectar el visor (Quest Link, SteamVR...) | Game.exe --vr (sin visor: --vr-sim muestra los dos ojos) | Blueprint: Get HMD Pose, Get Motion Controller Pose/Input, Play Haptic Effect
```
## VR
- La engine habla **OpenXR** directamente con el runtime instalado (Meta, SteamVR, WMR): no hace falta ningún SDK.
- Los dos ojos salen de un solo fotograma de la engine con **MSAA 4x** (lo que guarda un historial de pantalla está desactivado en VR para que los ojos no se mezclen); el primer ojo paga las simulaciones, la GI y las sombras, el segundo solo su vista. Meta: **por encima de 100 fps**. Teletransporte, manos, agarrar y el Quest independiente: el artículo **Juegos de VR** (`vr-quest`).
- Controles: **Get Motion Controller Pose** (lugar y dirección), **Get Motion Controller Input** (gatillo, grip, joystick, botones), **Play Haptic Effect** (vibración), **Get HMD Pose**, **Is VR Active**.
```blueprint
Event Tick -> Get Motion Controller Input ( 1 ) -> Branch ( Trigger > 0.5 ) -> Play Haptic Effect ( 1, 0.6, 0.1 )
```
## AR
- **Luz y color como los de la cámara**: la engine mide la exposición de la imagen, el balance de blancos (temperatura de color), la luz principal (de dónde viene y cuán fuerte) y la luz ambiente, y los aplica al sol, al cielo y a la exposición; los objetos virtuales reciben la luz real.
- En un teléfono el **giroscopio** gira la cámara (`--ar`). En un PC pruébalo con una foto: `Game.exe --ar-image photo.png`.

## AR con ARCore (Android)
```diagram steps
Project Settings > Android > Augmented Reality (ARCore) | Build > Package for Android | En el teléfono: permite la cámara, muévelo despacio sobre una superficie | El juego está en el mundo real: rastreado, en el suelo real, proyectando su sombra en él
```
- El teléfono se **rastrea con 6 grados de libertad**: la cámara del juego es la del teléfono, al caminar por la habitación.
- La **imagen de la cámara** está detrás del nivel; las cosas virtuales están delante con la luz del lugar (dirección, color e intensidad estimados por ARCore).
- Los **planos** que ARCore encuentra (suelos, mesas, paredes) se convierten en actores **AR Plane** con bloqueo: los Characters caminan sobre el suelo real. El suelo real **recibe las sombras** de las cosas virtuales (y el oscurecimiento de contacto bajo ellas).
- **Oclusión**: una cosa real más cercana que una virtual la oculta (el sofá delante del perro virtual), a partir de la profundidad de ARCore en los teléfonos que la tienen. Project Settings > Android > **Occlusion by Real Objects** (activada por defecto). En un PC, `REALENGINE_AR_SIM_FLOOR=60` sube 60 cm el suelo simulado: las bases de las cosas se esconden detrás.
- Listo para empezar: **New Project > Augmented Reality** (`BP_ARPlacer` coloca una caja donde tocas el suelo real; AR ya activada).
- El toque: **Event Touch** (Pressed / Released, Location en píxeles, Finger Index; en un PC el botón izquierdo del mouse es un dedo) y **Get Mouse Position**. Los dedos que tomó un widget táctil (joystick, botón) no lo disparan.
- Nodos: **Is AR Tracking**, **AR Line Trace** (un punto de la pantalla hacia el mundo real: coloca algo donde toca el jugador) y **Get AR Plane Count**.
- La AR es **opcional**: los teléfonos sin Google Play Services for AR ejecutan el juego como siempre. En un PC: `Game.exe --ar-image photo.png --ar-sim` la simula (la foto detrás, un suelo en el origen).
```blueprint
Event Touch (Pressed) -> Break Vector ( Location ) -> AR Line Trace ( X, Y ) -> Branch ( Hit ) -> Spawn Actor from Class ( BP_Chair, Location )
```
