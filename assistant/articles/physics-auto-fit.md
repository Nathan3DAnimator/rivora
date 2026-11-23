id: physics-auto-fit
kind: howto
title: Física automática pela malha: corpos do tamanho certo, cauda, orelhas, correntes | Automatic physics from the mesh: right-sized bodies, tails, ears, chains
keys: fisica automatica, física automática, gerar corpos, generate bodies, physics asset automatico, tamanho dos corpos, grossura da malha, mesh thickness, ragdoll automatico, corpos sobrepostos, bodies overlap, overlap, ragdoll explode, ragdoll tremendo, cauda fisica, tail physics, orelha de coelho, rabbit ear, bunny ear, corrente balançando, swinging chain, acessorio balançando, capa, saia, cape, skirt, ossos dinamicos, dynamic bones, predefinicoes de fisica, physics presets
--- pt
```diagram steps
Criar Physics Asset (ou Gerar Corpos) | Corpos pelos papéis do rig | Grossura medida na malha | Separados onde se sobrepõem | Cauda, orelhas, correntes viram Ossos Dinâmicos
```
## O que Gerar Corpos faz
- Cria os corpos pelos **papéis** do rig ([Detecção de rig](article:rig-detection)): quadril e peito como esferas, membros como cápsulas, pés deitados no chão. **Controles, alças de IK, polos, torções, dedos e cadeias** não ganham corpo.
- Mede a **grossura de cada parte na malha**: os vértices que cada osso mais move dizem o raio da cápsula. A coxa de um personagem gordo fica grossa, a de um magro fica fina.
- **Não deixa corpos se sobreporem** na pose de repouso (as duas coxas, o braço contra o peito): os que não são ligados por uma junta encolhem o necessário. Assim o ragdoll não explode nem treme ao começar.
- As **cadeias penduradas** viram **Ossos Dinâmicos** com a predefinição certa: **Cauda**, **Orelha** (a de coelho fica em pé e balança), **Cabelo**, **Corrente** (sem forma própria, balança solta), **Capa / Saia** (o vento leva) e **Brinco** para acessórios. Uma cadeia que você já ajustou fica como está.

## Predefinições (Ossos Dinâmicos)
- Selecione um osso no **Physics Asset > Ossos Dinâmicos > + Cadeia no osso selecionado** e escolha: Brinco, Cabelo, Cauda, Alça / Bolsinha, Bolsa Pesada, **Orelha**, **Corrente**, **Capa / Saia**. Todas colidem com os corpos do personagem.
--- en
```diagram steps
Create Physics Asset (or Generate Bodies) | Bodies by the rig's roles | Thickness measured on the mesh | Pushed apart where they overlap | Tail, ears, chains become Dynamic Bones
```
## What Generate Bodies does
- Makes the bodies by the rig's **roles** ([Rig detection](article:rig-detection)): hips and chest as spheres, limbs as capsules, feet lying on the ground. **Controls, IK handles, poles, twists, fingers and chains** get no body.
- Measures **each part's thickness on the mesh**: the vertices each bone moves most give its capsule's radius. A heavy character's thigh comes out thick, a slim one's thin.
- **Keeps bodies from overlapping** at the rest pose (the two thighs, the arm against the chest): bodies not joined by a joint shrink as much as needed. The ragdoll neither explodes nor jitters when it starts.
- The **hanging chains** become **Dynamic Bones** with the right preset: **Tail**, **Ear** (a rabbit's stands up and sways), **Hair**, **Chain** (no shape of its own, swings free), **Cape / Skirt** (the wind takes it) and **Earring** for accessories. A chain you already tuned is kept as it is.

## Presets (Dynamic Bones)
- Select a bone in **Physics Asset > Dynamic Bones > + Chain on the selected bone** and pick: Earring, Hair, Tail, Strap / Pouch, Heavy Bag, **Ear**, **Chain**, **Cape / Skirt**. All collide with the character's bodies.
