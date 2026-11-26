id: ragdoll
kind: recipe
title: Ragdoll e Physics Asset: ossos com física | Ragdoll and Physics Asset: bones with physics | Ragdoll y Physics Asset: huesos con física
keys: ragdoll, boneco de pano, physics asset, fisica dos ossos, bone physics, colisao dos ossos, bone collision, corpos, bodies, capsula, capsule, constraint, junta, joint, swing, twist, morrer, death, cair, fall, set simulate physics, set all bodies below simulate physics, physics blend weight, add impulse to bone, animacao fisica, physical animation, cabelo, hair, cauda, tail, braço solto, jiggle, balançar, hit reaction, reacao a golpe
--- pt
```diagram steps
Content Browser > Adicionar > IK Rig e Física > Physics Asset para: (o Skeletal Mesh) | Corpos gerados dos ossos (cápsulas, esferas, caixas) | Ajuste corpos e juntas (Swing, Twist) e teste com Simular | Detalhes do Skeletal Mesh > Physics Asset | Set Simulate Physics (Ragdoll) no Blueprint
```
## Como funciona
O **Physics Asset** (`.rphys`, como o editor de Physics Asset da Unreal) é a parte separada que diz **como os ossos colidem**:
- **Corpos**: uma cápsula, esfera ou caixa por osso, com tamanho, deslocamento e massa. *Gerar Corpos* faz tudo a partir dos ossos: um por osso longo, a bacia e o peito como esferas, os dedos e ossos de torção ficam no corpo de cima.
- **Juntas**: o limite de *Swing* (o cone, como um cotovelo ~70°, a coluna ~20°) e de *Twist* (o giro em torno do osso).
- **Simulado** ou **cinemático**: um corpo cinemático segue a animação e os outros ficam pendurados nele.
- **Animação física**: *Physical Animation* puxa o corpo para a pose animada: balança com golpes sem desabar.

A simulação é XPBD em subpassos (como a física moderna da Unreal): juntas firmes que não esticam nem explodem, colisão com o chão, as caixas do nível, os landscapes e entre os próprios corpos (os vizinhos não). 9 ragdolls custam cerca de 0,5 ms.

Três modos (Detalhes do Skeletal Mesh > *Simulate Physics*, ou nós):
- **Ragdoll**: todos os corpos.
- **Corpos abaixo de um osso**: uma cauda, cabelo, um braço atingido; o resto continua animado.
- **Physics Blend Weight**: mistura física e animação (de 1 a 0 para levantar).

```blueprint
Event BeginPlay -> Set Simulate Physics (Ragdoll) ( true ) -> Add Impulse to Bone ( Impulse, Bone Name )
```
```real
class Enemy extends Actor
    event BeginPlay()
        SetSimulatePhysicsRagdoll(true)
        AddImpulseToBone(Vector(0, 300, 200), "spine_03")
```
No editor, **Simular** derruba o esqueleto na prévia (Ctrl + clique dá um empurrão). Veja também [IK Rig](article:ik-rig).
--- en
```diagram steps
Content Browser > Add > IK Rig and Physics > Physics Asset for: (the Skeletal Mesh) | Bodies made from the bones (capsules, spheres, boxes) | Tune bodies and joints (Swing, Twist), test with Simulate | Skeletal Mesh Details > Physics Asset | Set Simulate Physics (Ragdoll) in a Blueprint
```
## How it works
The **Physics Asset** (`.rphys`, like Unreal's Physics Asset Editor) is the separate part that says **how the bones collide**:
- **Bodies**: a capsule, sphere or box per bone, with its size, offset and mass. *Generate Bodies* makes them from the bones: one per long bone, the pelvis and chest as spheres, fingers and twist bones riding the body above.
- **Joints**: the *Swing* limit (the cone: an elbow ~70°, the spine ~20°) and the *Twist* (the turn around the bone).
- **Simulated** or **kinematic**: a kinematic body follows the animation and the others hang from it.
- **Physical animation**: *Physical Animation* pulls the body toward the animated pose: it sways with hits without collapsing.

The simulation is XPBD in substeps (like Unreal's modern physics): firm joints that neither stretch nor explode, collisions with the floor, the level's boxes, landscapes and between its own bodies (not neighbors). 9 ragdolls cost about 0.5 ms.

Three modes (Skeletal Mesh Details > *Simulate Physics*, or nodes):
- **Ragdoll**: every body.
- **Bodies below a bone**: a tail, hair, a hit arm; the rest keeps animating.
- **Physics Blend Weight**: blends physics and animation (1 to 0 to stand back up).

```blueprint
Event BeginPlay -> Set Simulate Physics (Ragdoll) ( true ) -> Add Impulse to Bone ( Impulse, Bone Name )
```
```real
class Enemy extends Actor
    event BeginPlay()
        SetSimulatePhysicsRagdoll(true)
        AddImpulseToBone(Vector(0, 300, 200), "spine_03")
```
In the editor, **Simulate** drops the skeleton in the preview (Ctrl + click gives it a push). See also [IK Rig](article:ik-rig).
--- es
```diagram steps
Explorador de contenidos > Añadir > IK Rig and Physics > Physics Asset for: (la Skeletal Mesh) | Cuerpos hechos a partir de los huesos (cápsulas, esferas, cajas) | Ajusta cuerpos y articulaciones (Swing, Twist), prueba con Simulate | Detalles de la Skeletal Mesh > Physics Asset | Set Simulate Physics (Ragdoll) en un Blueprint
```
## Cómo funciona
El **Physics Asset** (`.rphys`, como el Physics Asset Editor de Unreal) es la parte separada que dice **cómo colisionan los huesos**:
- **Cuerpos**: una cápsula, esfera o caja por hueso, con su tamaño, desfase y masa. *Generate Bodies* los crea a partir de los huesos: uno por hueso largo, la pelvis y el pecho como esferas, los dedos y los huesos de torsión van con el cuerpo de arriba.
- **Articulaciones**: el límite *Swing* (el cono: un codo ~70°, la columna ~20°) y el *Twist* (el giro alrededor del hueso).
- **Simulado** o **cinemático**: un cuerpo cinemático sigue la animación y los demás cuelgan de él.
- **Animación física**: *Physical Animation* tira del cuerpo hacia la pose animada: se balancea con los golpes sin desplomarse.

La simulación es XPBD en subpasos (como la física moderna de Unreal): articulaciones firmes que ni se estiran ni explotan, colisiones con el suelo, las cajas del nivel, los landscapes y entre sus propios cuerpos (no los vecinos). 9 ragdolls cuestan cerca de 0,5 ms.

Tres modos (Detalles de la Skeletal Mesh > *Simulate Physics*, o nodos):
- **Ragdoll**: todos los cuerpos.
- **Cuerpos bajo un hueso**: una cola, el pelo, un brazo golpeado; el resto sigue animándose.
- **Physics Blend Weight**: mezcla física y animación (de 1 a 0 para volver a ponerse de pie).

```blueprint
Event BeginPlay -> Set Simulate Physics (Ragdoll) ( true ) -> Add Impulse to Bone ( Impulse, Bone Name )
```
```real
class Enemy extends Actor
    event BeginPlay()
        SetSimulatePhysicsRagdoll(true)
        AddImpulseToBone(Vector(0, 300, 200), "spine_03")
```
En el editor, **Simulate** suelta el esqueleto en la vista previa (Ctrl + clic le da un empujón). Mira también [IK Rig](article:ik-rig).
