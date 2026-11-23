id: vr-base-hand
kind: howto
title: Mão base do VR: exportar em FBX para modelar e riggar, e usar | VR base hand: export as FBX to model and rig, and use
keys: mao base, mão base, mao do vr, mão do vr, maos do vr, mãos do vr, exportar mao, exportar mão, mao em fbx, mão em fbx, malha da mao, malha da mão, skeletal mesh da mao, mao com esqueleto, mão com esqueleto, ossos da mao, ossos da mão, dedos, juntas do openxr, rigger, modelador, base hand, vr hand, vr hands, hand mesh, hand fbx, export base hand, use the base hand, hand skeleton, hand bones, finger bones, openxr joints, hand rig, rig a hand, mano base, mano de vr, exportar mano, huesos de la mano, fist, pinch, point, grip, thumbs up
--- pt
```diagram steps
Project Settings > VR > Exportar Mão Base (FBX) | Modelador e rigger refazem a mão (mesmos nomes de ossos) | Importar o FBX no Content Browser | Malha da Mão Esquerda/Direita = a Skeletal Mesh nova
```
- **Exportar Mão Base (FBX)...** (Project Settings > VR, em Malhas das Mãos) grava numa pasta `RealEngine_BaseHand_L.fbx`, `RealEngine_BaseHand_R.fbx` e um leia-me. Cada FBX tem:
  - uma **malha com pele** (palma, dedos e polegar, ~4 mil triângulos, centímetros, Z para cima);
  - o **esqueleto das 26 juntas do OpenXR**: `Wrist`, `Palm` e, para Thumb, Index, Middle, Ring e Little, os ossos `Metacarpal`, `Proximal`, `Intermediate` (menos no polegar), `Distal` e `Tip`, com o sufixo `_L` ou `_R`;
  - **7 poses** como animações (30 fps, do quadro 0 aberto ao quadro 10 na pose): Open, Relaxed, Fist, Grip, Point, Pinch, ThumbsUp.
- Abre no **Blender** e no **Maya** com os ossos, os pesos e as animações. Entregue ao modelador e ao rigger: a malha pode mudar à vontade; **mantenha os nomes dos ossos** (o sufixo pode mudar e "Pinky" vale por Little). É por eles que a engine move os dedos.
- **Usar a Mão Base** importa a mão para `/Content/VR/Hands` e já a coloca nas duas mãos: dá para testar na hora com **Play > Prévia em VR**.
- Para usar a mão nova: importe o FBX no Content Browser e escolha a Skeletal Mesh em **Malha da Mão Esquerda** e **Malha da Mão Direita**.
- Como os dedos se movem:
  - **Mãos rastreadas** (sem controle): cada osso aponta para a junta real seguinte, e a mão fica no pulso e nos nós dos dedos de verdade.
  - **Controle**: o gatilho fecha o indicador, o grip fecha os outros e os botões A/B/X/Y baixam o polegar. A engine mistura as animações **Open** e **Fist** da sua mão, dedo por dedo (ela as acha pelo nome). Sem essas animações, dobra cada dedo pelos eixos dos próprios ossos.
- Uma **Static Mesh** (.rmesh) continua valendo como mão rígida. **Escala** e **Rotação da Malha da Mão** ajustam as duas.
- Custo: ~0,0015 ms por mão por quadro (os dedos resolvidos na CPU, a pele deformada na GPU como a de qualquer personagem).
- Veja também [Jogos em VR](article:vr-quest).
--- en
```diagram steps
Project Settings > VR > Export Base Hand (FBX) | A modeler and a rigger redo the hand (same bone names) | Import the FBX in the Content Browser | Left/Right Hand Mesh = the new Skeletal Mesh
```
- **Export Base Hand (FBX)...** (Project Settings > VR, under Hand Meshes) writes `RealEngine_BaseHand_L.fbx`, `RealEngine_BaseHand_R.fbx` and a read-me to a folder. Each FBX has:
  - a **skinned mesh** (palm, fingers and thumb, ~4k triangles, centimeters, Z up);
  - the **skeleton of OpenXR's 26 hand joints**: `Wrist`, `Palm` and, for Thumb, Index, Middle, Ring and Little, the bones `Metacarpal`, `Proximal`, `Intermediate` (not on the thumb), `Distal` and `Tip`, suffixed `_L` or `_R`;
  - **7 poses** as animations (30 fps, from open at frame 0 to the pose at frame 10): Open, Relaxed, Fist, Grip, Point, Pinch, ThumbsUp.
- It opens in **Blender** and **Maya** with its bones, weights and animations. Hand it to a modeler and a rigger: the mesh may change freely; **keep the bones' names** (the suffix may change, and "Pinky" counts as Little). They are how the engine moves the fingers.
- **Use the Base Hand** imports the hand into `/Content/VR/Hands` and sets it on both hands: try it at once with **Play > VR Preview**.
- To use the new hand: import the FBX in the Content Browser and pick the Skeletal Mesh in **Left Hand Mesh** and **Right Hand Mesh**.
- How the fingers move:
  - **Tracked hands** (no controller): each bone points at the next real joint, and the hand sits at the real wrist and knuckles.
  - **Controllers**: the trigger closes the index, the grip the others, and the A/B/X/Y buttons lower the thumb. The engine blends your hand's **Open** and **Fist** animations finger by finger (it finds them by name). Without them, each finger bends around its own bones' axes.
- A **Static Mesh** (.rmesh) still works as a rigid hand. **Hand Mesh Scale** and **Rotation** adjust both kinds.
- Cost: ~0.0015 ms per hand per frame (the fingers solved on the CPU, the skin deformed on the GPU like any character's).
- See also [VR games](article:vr-quest).
