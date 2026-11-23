id: metro-scene
kind: howto
title: Cena do metrô abandonado (raios de sol, trem, mato) | Abandoned metro scene (sun shafts, train, weeds)
keys: metro, metrô, metro abandonado, estacao abandonada, tunel, trem, vagao, trilho, trilhos, dormente, raios de sol, god rays, feixes de luz, luz pelo telhado, poeira no ar, pocas, mato, grafite, abandoned metro, subway, train car, rails, sun shafts, light beams, broken roof, puddles, weeds, graffiti, scene metro
--- pt
```diagram steps
Criar projeto (Primeira Pessoa) | --scene metro | Mapa MetroAbandonado | Play anda pelo salão
```
- **Abrir**: `realengine --new-project "<Projetos>/MetroAbandonado" --template 2 --scene metro`. O nível vai para `Content/Maps/MetroAbandonado.rlevel` e vira o mapa inicial do editor e do jogo (no Alto).
- **O que mostra**: um salão de metrô de 64 m com o telhado quebrado. O sol entra pelas frestas e vira **feixes na neblina volumétrica**; um trem velho de dois vagões (pintura descascada, ferrugem, janelas sujas, grafite) no trilho da direita; chão de concreto molhado com poças; mato, samambaias e capim entre os trilhos.
- **Do que é feito** (tudo da engine, para estudar):
  - **Trilhos**: um **Spline Mesh** com o perfil dos dois trilhos e **Spline Instances** para os dormentes, mais outro Spline Mesh de brita. Mova os pontos do spline e os trilhos curvam juntos.
  - **Mato**: um ator **Foliage** (capim feito pela cena, ervas e samambaias CC0) pintado onde o sol cai. Selecione e use o pincel para pintar mais.
  - **Poças, grafite e escorridos**: **Decals** (Adicionar > Efeitos Visuais > Decal). O chão tem o nó **Wet Surface**.
  - **Luz**: Directional Light alta entrando pelas frestas, Sky Light fraca, **Exponential Height Fog** volumétrica (densidade 0,009, anisotropia 0,8) e um Post Process com exposição fixa, GI mais forte e uma **Color Grading LUT** (sombras frias, luz quente).
  - **Feixes nas poças**: superfícies quase espelhadas (poças, filme molhado, vidro) refletem também a neblina iluminada que o raio do reflexo atravessa: os feixes de sol aparecem no chão molhado.
- **Dica**: os feixes aparecem melhor quando as frestas são compridas e estreitas e o sol atravessa a vista de lado (os feixes ficam "de perfil").
- Os assets são CC0 (Poly Haven, ambientCG), baixados no build; a lista e as licenças ficam em `Content/Demo/LICENSES.txt`. Veja também [Jogos de demonstração](article:demo-games) e [Céu, nuvens e neblina](article:sky-fog).
--- en
```diagram steps
Create project (First Person) | --scene metro | MetroAbandonado map | Play walks the hall
```
- **Open it**: `realengine --new-project "<Projects>/MetroAbandonado" --template 2 --scene metro`. The level goes to `Content/Maps/MetroAbandonado.rlevel` and becomes the editor's and the game's startup map (on High).
- **What it shows**: a 64 m metro hall with a broken roof. The sun comes through the gaps as **shafts in the volumetric fog**; an old two-car train (peeling paint, rust, dirty windows, graffiti) on the right track; wet concrete with puddles; weeds, ferns and grass between the rails.
- **What it is made of** (all the engine's, to learn from):
  - **Rails**: a **Spline Mesh** with the two rails' profile and **Spline Instances** for the sleepers, plus another Spline Mesh of gravel. Move the spline's points and the rails bend along.
  - **Weeds**: a **Foliage** actor (grass made by the scene, CC0 weeds and ferns) painted where the sun falls. Select it and use the brush to paint more.
  - **Puddles, graffiti and streaks**: **Decals** (Add > Visual Effects > Decal). The floor has the **Wet Surface** node.
  - **Light**: a high Directional Light through the gaps, a dim Sky Light, a volumetric **Exponential Height Fog** (density 0.009, anisotropy 0.8) and a Post Process with a fixed exposure, stronger GI and a **Color Grading LUT** (cool shade, warm light).
  - **Shafts in the puddles**: near-mirror surfaces (puddles, a wet film, glass) also reflect the lit fog their reflection ray crosses: the sun's shafts show on the wet floor.
- **Tip**: shafts read best when the gaps are long and narrow and the sun crosses the view from the side (the shafts seen edge on).
- The assets are CC0 (Poly Haven, ambientCG), downloaded at build time; their list and licenses go in `Content/Demo/LICENSES.txt`. See also [Demo games](article:demo-games) and [Sky, clouds and fog](article:sky-fog).
