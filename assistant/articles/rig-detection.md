id: rig-detection
kind: howto
title: Detecção de rig: Mixamo, Rigify, Auto-Rig Pro e outros prontos | Rig detection: Mixamo, Rigify, Auto-Rig Pro and more, ready to use
keys: deteccao de rig, detecção de rig, rig detection, reconhecer rig, auto rig, autorig, mixamo, rigify, auto-rig pro, arp, accurig, character creator, daz, metahuman, vroid, biped, rig do tlender, tlender rig, ik e fk, ik fk, hierarquia do rig, nomes dos ossos, bone names, foot ik nao funciona, leg ik nao acha o pe, pe flutuando, feet floating, pe nao fica no chao, papeis dos ossos, bone roles, analisar rig, analyze rig, rig detectado
--- pt
```diagram steps
Importar o personagem (FBX, glTF, .tlend, .rasset) | A engine descobre o que cada osso é | Rig Detectado: sistema, classe, papéis, cadeias | Leg IK / Foot IK e Gerar Corpos já funcionam
```
## O que a engine reconhece
- Os mesmos rigs que o Tlender reconhece: o **auto-rig do Tlender**, **Rigify**, **Auto-Rig Pro**, **AdvancedSkeleton**, **Mixamo / HumanIK**, **UE / MetaHuman**, **VRoid**, **Character Creator / AccuRig**, **Daz**, **Biped / CAT**, **SMPL**, **Valve** e **MPFB**. Um rig sem nomes conhecidos é lido pela **forma do esqueleto**: o quadril onde duas pernas e a coluna se encontram, o peito onde estão os braços e o pescoço.
- Cada osso ganha um papel: **quadril, coluna, peito, pescoço, cabeça, clavícula, braço, antebraço, mão, dedos, coxa, canela, pé, ponta do pé**. Também separa os **controles** (FK, alças de IK, polos, MCH), as **torções** e as **cadeias penduradas** (cauda, orelhas, cabelo, correntes, capas, acessórios).
- Num rig de controle (Rigify, Auto-Rig Pro), os ossos que **deformam** são os usados; os controles não viram corpo de física.

## Onde ver
- Abra o **Skeletal Mesh**: embaixo da **Árvore do Esqueleto** fica **Rig Detectado** com o sistema, a classe (Humanoide, Quadrúpede...), quantos papéis achou, as cadeias e a lista **Papéis** (um clique seleciona o osso). **Analisar Rig** refaz.

## O que já funciona sozinho
- Todo lugar que pede um osso pelo nome da Unreal (`foot_l`, `calf_r`, `thigh_l`, `spine_02`, `head`...) acha o osso certo em **qualquer** rig. O nó **Leg IK** e o **Foot IK** do AnimGraph vêm com `foot_l` / `foot_r`: num personagem do Mixamo eles já pegam `LeftFoot` / `RightFoot`. **Basta ligar** o Foot IK para os pés ficarem no chão, sem flutuar.
- O **Aim Offset**, os **estilos de locomoção**, a **cobertura**, as **reações a golpe** e o **Motion Matching** também acham os ossos pelos papéis.
- **Gerar Corpos** (Physics Asset) usa os papéis e a malha: veja [Física automática pela malha](article:physics-auto-fit).
--- en
```diagram steps
Import the character (FBX, glTF, .tlend, .rasset) | The engine finds what each bone is | Rig Detected: system, class, roles, chains | Leg IK / Foot IK and Generate Bodies just work
```
## What the engine recognizes
- The same rigs the Tlender recognizes: the **Tlender's auto-rig**, **Rigify**, **Auto-Rig Pro**, **AdvancedSkeleton**, **Mixamo / HumanIK**, **UE / MetaHuman**, **VRoid**, **Character Creator / AccuRig**, **Daz**, **Biped / CAT**, **SMPL**, **Valve** and **MPFB**. A rig with no known names is read by the **skeleton's shape**: the hips where two legs and the spine meet, the chest where the arms and the neck are.
- Each bone gets a role: **hips, spine, chest, neck, head, clavicle, upper arm, forearm, hand, fingers, thigh, shin, foot, toe**. It also tells apart the **controls** (FK, IK handles, poles, MCH), the **twists** and the **hanging chains** (tail, ears, hair, chains, capes, accessories).
- In a control rig (Rigify, Auto-Rig Pro) the **deforming** bones are the ones used; controls never become physics bodies.

## Where to see it
- Open the **Skeletal Mesh**: under the **Skeleton Tree** is **Rig Detected** with the system, the class (Humanoid, Quadruped...), how many roles it found, the chains and the **Roles** list (a click selects the bone). **Analyze Rig** redoes it.

## What works by itself
- Everything that asks for a bone by its Unreal name (`foot_l`, `calf_r`, `thigh_l`, `spine_02`, `head`...) finds the right bone on **any** rig. The AnimGraph's **Leg IK** node and **Foot IK** come with `foot_l` / `foot_r`: on a Mixamo character they already take `LeftFoot` / `RightFoot`. **Just switch on** Foot IK and the feet stay on the ground, no floating.
- The **Aim Offset**, the **locomotion styles**, **cover**, **hit reactions** and **Motion Matching** find their bones by role too.
- **Generate Bodies** (Physics Asset) uses the roles and the mesh: see [Automatic physics from the mesh](article:physics-auto-fit).
