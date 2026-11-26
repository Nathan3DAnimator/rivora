id: clothing
kind: howto
title: Roupas de tecido no personagem: camisa, saia e capa | Cloth garments on a character: shirt, skirt and cape | Prendas de tela en el personaje: camisa, falda y capa
keys: roupa, roupas, clothing, garment, vestido, dress, camisa, shirt, saia, skirt, capa, cape, manto, cloak, tecido no personagem, cloth on character, roupa atravessando, clothes clipping, roupa entrando no corpo, cloth going through the body, colisao com o corpo, body collision, physics asset, preso pela cintura, pinned at the waist, corrida, run
--- pt
Uma roupa de **tecido** (o simulador VBD na GPU) vestida num personagem colide com os **corpos do Physics Asset** dele — cápsulas e esferas na pose de cada quadro, também em personagens com skin — e não atravessa o corpo nem numa corrida: cada cápsula é varrida entre o lugar do quadro anterior e o atual nos subpassos, e um vértice que o membro passou é empurrado para o lado de onde veio.
- **Vestir**: uma malha de roupa vira tecido (**Criar Tecido a partir da Seleção**, ou o componente Tecido com a malha), **Seguir Ator / Osso**: o personagem e o osso que a carrega (a cintura para a saia, o peito para a camisa e a capa). O Skeletal Mesh precisa de um **Physics Asset**.
- **Presets**: **Camisa** e **Saia** (finas contra o corpo, dois subpassos, presas na faixa de cima girando com o osso), **Capa**.
- **Presa**: os vértices da faixa de cima (Faixa Presa, cm) seguem o osso; o resto cai e balança.
- Medida: numa corrida, a saia e a capa ficam fora do corpo (no pior quadro um vértice a menos de 2 cm; 0,002% dos vértices a mais de 1 cm).

```diagram steps
Skeletal Mesh + Physics Asset | malha da roupa | Criar Tecido a partir da Seleção | Preset: Saia | Seguir Osso: pelvis | Play
```
--- en
A **cloth** garment (the VBD solver on the GPU) worn by a character collides with its **Physics Asset's bodies** — capsules and spheres at each frame's pose, skinned characters too — and stays out of the body even at a run: each capsule is swept from last frame's place to this one's over the substeps, and a vertex a limb passed is pushed back to the side it came from.
- **Wear it**: a garment's mesh made a cloth (**Make Cloth from Selected**, or the Cloth component with the mesh), **Follow Actor / Bone**: the character and the bone that carries it (the waist for a skirt, the chest for a shirt and a cape). The Skeletal Mesh needs a **Physics Asset**.
- **Presets**: **Shirt** and **Skirt** (thin against the body, two substeps, pinned at their top band turning with the bone), **Cape**.
- **Pinned**: the top band's vertices (Pin Band, cm) follow the bone; the rest falls and swings.
- Measured: at a run the skirt and the cape stay out of the body (at the worst frame one vertex under 2 cm in; 0.002% of the vertices over 1 cm).

```diagram steps
Skeletal Mesh + Physics Asset | the garment's mesh | Make Cloth from Selected | Preset: Skirt | Follow Bone: pelvis | Play
```
--- es
Una prenda de **tela** (el solver VBD en la GPU) puesta en un personaje colisiona con los **cuerpos de su Physics Asset** (cápsulas y esferas en la pose de cada fotograma, también en personajes con skin) y se mantiene fuera del cuerpo incluso corriendo: cada cápsula se barre desde su lugar del fotograma anterior hasta el de este a lo largo de los subpasos, y un vértice que una extremidad atravesó es empujado de vuelta al lado de donde vino.
- **Ponerla**: la malla de una prenda convertida en tela (**Make Cloth from Selected**, o el componente Cloth con la malla), **Follow Actor / Bone**: el personaje y el hueso que la lleva (la cintura para una falda, el pecho para una camisa y una capa). El Skeletal Mesh necesita un **Physics Asset**.
- **Presets**: **Shirt** y **Skirt** (finas contra el cuerpo, dos subpasos, fijas en su banda superior que gira con el hueso), **Cape**.
- **Pinned**: los vértices de la banda superior (Pin Band, cm) siguen el hueso; el resto cae y se balancea.
- Medido: corriendo, la falda y la capa se mantienen fuera del cuerpo (en el peor fotograma un vértice a menos de 2 cm dentro; 0,002% de los vértices a más de 1 cm).

```diagram steps
Skeletal Mesh + Physics Asset | la malla de la prenda | Make Cloth from Selected | Preset: Skirt | Follow Bone: pelvis | Jugar
```
