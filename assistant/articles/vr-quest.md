id: vr-quest
kind: howto
title: Jogos em VR: Quest 3S, teleporte, mãos, pegar e arremessar | VR games: Quest 3S, teleport, hands, grab and throw
keys: vr, maos fisicas, mãos físicas, physical hands, manos fisicas, colisao das maos, colisão das mãos, hand collision, empurrar com a mao, push with hand, gesto de pegar, grab gesture, set vr hand collision, realidade virtual, virtual reality, realidad virtual, quest, quest 3, quest 3s, meta quest, oculus, headset, oculos vr, gafas vr, standalone, autonomo, autônomo, quest link, air link, steamvr, openxr, vr preview, previa em vr, prévia em vr, iniciar em vr, start in vr, empacotar para meta quest, package for meta quest, apk quest, teleporte, teleport, teletransporte, arco de teleporte, teleport arc, giro por passos, snap turn, giro suave, smooth turn, locomocao, locomoção, locomotion, vinheta de conforto, comfort vignette, enjoo, motion sickness, mareo, maos, mãos, hands, manos, hand tracking, rastreamento das maos, rastreamento das mãos, pinca, pinça, pinch, pegar objeto, grab, agarrar, arremessar, throw, lanzar, pegavel, pegável, grabbable, encaixe, socket, grab socket, laser, ponteiro laser, laser pointer, menu em vr, vr menu, widget em vr, foveated, foveacao, foveação, foveated rendering, renderizacao foveada, ffr, spacewarp, application spacewarp, 72 hz, 90 hz, 120 hz, taxa de atualizacao, refresh rate, vr template, template vr, pawn vr, vr pawn, event on vr grabbed, vr teleport, vr grab
--- pt
```diagram steps
Novo Projeto > Realidade Virtual (ou Project Settings > VR > Iniciar em VR) | Play > Prévia em VR: o nível no headset pelo Quest Link / Air Link | Ajuste locomoção, giro, mãos e qualidade em Project Settings > VR | Compilar > Empacotar para Meta Quest: o APK roda no Quest sozinho
```
## Começar
- **Novo Projeto > Realidade Virtual**: uma sala pronta como o VR Template da Unreal: teleporte, giro por passos, mãos, um laser que usa a porta e a alavanca, cubos para pegar e arremessar, uma bola segurada pelo cabo e um **Encaixe de Pegar** na mesa.
- **Play > Prévia em VR** joga o nível aberto no headset (Quest Link por cabo, Air Link ou SteamVR), com as mudanças não salvas. Sem headset, abre no monitor.
- O jogo empacotado abre no headset quando **Project Settings > VR > Iniciar em VR** está ligado. Para o **Quest sozinho** (sem PC): **Compilar > Empacotar para Meta Quest**. No headset: modo desenvolvedor ligado e a depuração USB autorizada; instale com `adb install -r Jogo_Quest.apk`.

## Controles do jogador (o pawn VR)
- **Analógico esquerdo**: empurre para frente, aparece o **arco de teleporte** (azul num chão, vermelho numa parede); solte para ir. A cabeça cai em cima do ponto.
- **Analógico direito**: **giro por passos** (45° em volta da cabeça) ou **giro suave**. Em Teleporte e Suave, o esquerdo anda e o direito, empurrado para frente, teleporta.
- **Grip**: pega o que tem um **Interagível** marcado como **Pegável**; soltando, ele é **arremessado com a velocidade da mão** (a física volta) ou **encaixa** num Encaixe de Pegar a até 20 cm (Tag Exigida: só o que tem aquela Gameplay Tag). **Encaixar na Mão**: segurado como foi feito (uma arma pelo cabo).
- **Gatilho**: usa o Interagível que o **laser** aponta (portas, alavancas, botões: os mesmos do jogo normal) e clica nos menus. Um menu (pausa, opções) aparece num **painel** 1,2 m à frente; o laser é o mouse.
- **Mãos rastreadas** (Project Settings > VR > Rastreamento das Mãos): a pinça é o gatilho, o punho fechado é o grip. **Gesto de Pegar** e **Gesto de Arrastar**: Punho, Pinça, Qualquer Dedo no Indicador (o polegar ou o médio na metade de cima do indicador) ou Punho ou Qualquer Dedo (padrão). Cada dedo à parte e a forma da mão: Get VR Finger Curls, Get VR Finger Touch, Get VR Hand Gesture.
- **Mãos Físicas** (opcional): as mãos desenhadas param em paredes, chão e mesas; com **Mãos Colidem com Objetos**, também em corpos com física e Interagíveis, os dedos inclusive. **Mãos Empurram Corpos**: encostar num corpo com física o empurra, o quanto a mão real entra nele, até pousando devagar. **Set VR Hand Collision** troca as três em jogo.
- **Vinheta de Conforto**: escurece a borda da visão ao andar e girar (menos enjoo).

## Qualidade e desempenho no Quest
- **Qualidade em VR > Quest (autônomo)**: luz indireta pelos **lightmaps e pelo volume assado** (Compilar > Build Lighting antes de empacotar), MSAA 4×, nada por raio, sem efeitos de tela. Os dois olhos dividem o trabalho do quadro (simulações, GI e sombras uma vez só).
- **Renderização Foveada** (Baixa, Média, Alta): as bordas de cada olho sombreadas mais grosso. A Alta tirou um terço do tempo de GPU na cena da casa. **Foveação Dinâmica**: só aprofunda quando falta tempo.
- **Taxa de Atualização** 72/90/120 Hz: em 120 Hz o jogo tem 8,3 ms para os dois olhos. Comece em 90 e meça (F10, Debug Viewer).

```blueprint
Event On VR Grabbed ( Hand ) -> Play Haptic Effect ( Hand, 0.5, 0.05 )
Event BeginPlay -> Set VR Hand Collision ( true, false, true )
Event On VR Released ( Hand, Velocity, Socket ) -> Is Valid ( Socket ) -> Branch -> Print String ( "Encaixou!" )
Event BeginPlay -> Set VR Locomotion ( 2, 0 ) -> Set VR Comfort Vignette ( 0.4 )
```
--- en
```diagram steps
New Project > Virtual Reality (or Project Settings > VR > Start in VR) | Play > VR Preview: the level on the headset through Quest Link / Air Link | Set locomotion, turning, hands and quality in Project Settings > VR | Build > Package for Meta Quest: the APK runs on the Quest by itself
```
## Getting started
- **New Project > Virtual Reality**: a ready room like Unreal's VR Template: teleport, snap turns, hands, a laser that uses the door and the lever, cubes to grab and throw, a ball held by its grip and a **Grab Socket** on the table.
- **Play > VR Preview** plays the open level on the headset (Quest Link by cable, Air Link or SteamVR), unsaved changes too. No headset: it opens on the monitor.
- The packaged game opens on the headset when **Project Settings > VR > Start in VR** is on. For the **Quest standalone** (no PC): **Build > Package for Meta Quest**. On the headset: developer mode on and USB debugging allowed; install with `adb install -r Game_Quest.apk`.

## The player's controls (the VR pawn)
- **Left stick**: push forward and the **teleport arc** shows (blue on a floor, red on a wall); let go to go. The head lands over the spot.
- **Right stick**: **snap turn** (45° around the head) or **smooth turn**. With Teleport and Smooth, the left stick walks and the right one, pushed forward, teleports.
- **Grip**: takes what has an **Interactable** set **Grabbable**; let go, it is **thrown with the hand's speed** (its physics back) or **snaps** into a Grab Socket within 20 cm (Required Tag: only what has that Gameplay Tag). **Snap to Hand**: held as made (a gun by its grip).
- **Trigger**: uses the Interactable the **laser** points at (doors, levers, buttons: the same as the flat game's) and clicks menus. A menu (pause, options) shows on a **panel** 1.2 m ahead; the laser is the mouse.
- **Tracked hands** (Project Settings > VR > Hand Tracking): a pinch is the trigger, a fist the grip. **Grab Gesture** and **Drag Gesture**: Fist, Pinch, Any Finger to Index (the thumb or the middle on the index's upper half) or Fist or Any Finger (the default). Each finger alone and the hand's shape: Get VR Finger Curls, Get VR Finger Touch, Get VR Hand Gesture.
- **Physical Hands** (optional): the drawn hands stop at walls, floors and tables; with **Hands Collide with Objects**, at bodies with physics and Interactables too, the fingers included. **Hands Push Bodies**: touching a body with physics pushes it, as deep as the real hand goes in, even laid on it slowly. **Set VR Hand Collision** changes the three in game.
- **Comfort Vignette**: darkens the view's edge while walking and turning (less motion sickness).

## Quality and performance on the Quest
- **VR Quality > Quest (standalone)**: indirect light from the **lightmaps and the baked volume** (Build > Build Lighting before packaging), MSAA 4x, nothing traced, no screen-space effects. Both eyes share the frame's work (simulations, GI and shadows once).
- **Foveated Rendering** (Low, Medium, High): each eye's edges shaded coarser. High cut a third of the GPU's time in the house scene. **Dynamic Foveation**: deeper only when time runs short.
- **Refresh Rate** 72/90/120 Hz: at 120 Hz the game has 8.3 ms for both eyes. Start at 90 and measure (F10, Debug Viewer).

```blueprint
Event On VR Grabbed ( Hand ) -> Play Haptic Effect ( Hand, 0.5, 0.05 )
Event BeginPlay -> Set VR Hand Collision ( true, false, true )
Event On VR Released ( Hand, Velocity, Socket ) -> Is Valid ( Socket ) -> Branch -> Print String ( "Snapped!" )
Event BeginPlay -> Set VR Locomotion ( 2, 0 ) -> Set VR Comfort Vignette ( 0.4 )
```
--- es
```diagram steps
Nuevo Proyecto > Realidad Virtual (o Project Settings > VR > Iniciar en VR) | Play > Vista previa en VR: el nivel en el visor por Quest Link / Air Link | Ajusta locomoción, giro, manos y calidad en Project Settings > VR | Compilar > Empaquetar para Meta Quest: el APK corre en el Quest solo
```
## Empezar
- **Nuevo Proyecto > Realidad Virtual**: una sala lista como el VR Template de Unreal: teletransporte, giro por pasos, manos, un láser que usa la puerta y la palanca, cubos para agarrar y lanzar, una pelota sujeta por su mango y un **Encaje de Agarre** en la mesa.
- **Play > Vista previa en VR** juega el nivel abierto en el visor (Quest Link por cable, Air Link o SteamVR), con los cambios sin guardar. Sin visor, se abre en el monitor.
- El juego empaquetado se abre en el visor con **Project Settings > VR > Iniciar en VR**. Para el **Quest autónomo** (sin PC): **Compilar > Empaquetar para Meta Quest**. En el visor: modo desarrollador activo y la depuración USB autorizada; instala con `adb install -r Juego_Quest.apk`.

## Controles del jugador (el pawn VR)
- **Stick izquierdo**: empújalo hacia adelante y aparece el **arco de teletransporte** (azul en un suelo, rojo en una pared); suéltalo para ir.
- **Stick derecho**: **giro por pasos** (45° alrededor de la cabeza) o **giro suave**.
- **Grip**: agarra lo que tiene un **Interactuable** marcado como **Agarrable**; al soltarlo, se **lanza con la velocidad de la mano** o **encaja** en un Encaje de Agarre a menos de 20 cm.
- **Gatillo**: usa el Interactuable al que apunta el **láser** (puertas, palancas, botones) y hace clic en los menús, que aparecen en un **panel** a 1,2 m.
- **Manos rastreadas**: la pinza es el gatillo, el puño cerrado el grip. **Gesto de Agarrar** y **de Arrastrar**: Puño, Pinza, Cualquier Dedo en el Índice o Puño o Cualquier Dedo. **Manos Físicas** (opcional): se detienen en paredes y mesas; con **Colisión con Objetos**, también en cuerpos con física e Interactuables. **Las Manos Empujan Cuerpos**: tocar un cuerpo con física lo empuja, aunque sea despacio. **Set VR Hand Collision** cambia las tres en el juego. **Viñeta de Confort**: oscurece el borde al moverse (menos mareo).

## Calidad y rendimiento en el Quest
- **Calidad en VR > Quest (autónomo)**: luz indirecta por **lightmaps y el volumen horneado**, MSAA 4x, nada por rayos. Los dos ojos comparten el trabajo del cuadro.
- **Renderizado Foveado** (Baja, Media, Alta): los bordes de cada ojo sombreados más gruesos; la Alta quitó un tercio del tiempo de GPU en la escena de la casa.
- **Frecuencia** 72/90/120 Hz: a 120 Hz hay 8,3 ms para los dos ojos.

```blueprint
Event On VR Grabbed ( Hand ) -> Play Haptic Effect ( Hand, 0.5, 0.05 )
Event BeginPlay -> Set VR Hand Collision ( true, false, true )
Event BeginPlay -> Set VR Locomotion ( 2, 0 ) -> Set VR Comfort Vignette ( 0.4 )
```
