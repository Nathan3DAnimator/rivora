id: hair-groom
kind: concept
title: Cabelo em fios (Groom) | Hair in strands (Groom)
keys: cabelo de verdade em fios, cabelo em fios no personagem, cabelo, hair, fios, strands, groom, groom component, componente groom, cabelo realista, realistic hair, pelo, fur, barba, beard, marschner, melanina, melanin, ruivo, red hair, loiro, blond, cacheado, curly, ondulado, wavy, cabelo comprido, long hair, hair cards, cards de cabelo, sombra do cabelo, hair shadow, REALENGINE_NO_HAIR, cabelo dinâmico, dynamic hair, física do cabelo, hair physics, cabelo balançando, hair swinging, vento no cabelo, wind in hair, colisão do cabelo, hair collision, importar cabelo do tlender, import tlender hair, guias, guides, rgroom, curvas obj, obj curves, polilinhas
--- pt
```diagram steps
Adicione o componente Groom ao ator da cabeça
Os fios nascem no topo da malha, penteados para trás
Gravidade, cachos e mechas dão a forma
Cada fio é iluminado como fibra (brilho duplo, luz atravessando)
```
O **Groom** (como o Groom Component da Unreal) faz crescer cabelo em **fios de verdade** sobre a malha do ator, em vez de cartões com textura.
- **Como usar:** selecione o ator da cabeça, **+ Adicionar > Groom**. Escolha um **Preset** (Curto, Comprido, Ondulado, Cacheado) e ajuste: **Strands** (quantos fios), **Length**, **Curl** e **Curl Frequency** (cachos), **Gravity** (quanto cai), **Clumping** (mechas), **Scalp** (até onde desce na cabeça) e **Hairline** (a testa: o ator olha para +X).
- **Cor:** **Melanin** (0 loiro claro ... 1 preto) e **Redness** (ruivo), como na Unreal; **Dye** tinge por cima; **Roughness** alarga o brilho.
- **Luz:** os três brilhos de um fio (o branco, o colorido mais abaixo e a luz atravessando contra o sol) e a sombra que o cabelo faz em si mesmo (uma grade de densidade).
- **Longe:** com **LOD** ligado, menos fios e mais largos: a mesma aparência por menos trabalho.
- **Custo** (RTX 3060, Épico): ~0,3-0,7 ms com 20 mil fios numa cabeça perto da câmera.
- **Dinâmico (física):** ligue **Dynamic** e o cabelo é simulado na GPU: cai, balança quando a cabeça mexe, o vento o move e ele **colide** com a cabeça, os ossos do personagem (Skeletal Mesh: ombros, costas, braços; com Physics Asset, nos raios dos corpos) e as caixas do cenário. **Stiffness** segura o penteado (baixo: solto), **Damping** acalma, **Wind** (cm/s²) e **Gusts** (rajadas) dão o vento, **Collision Radius** o volume. Desligado, fica como cresceu, sem custo.
- **Do Tlender:** importe o `.tlend` (Arquivo > Importar): cada hair de fios vira um `.rgroom` em `Imported/Hair` e um Groom no ator da superfície, com a contagem, a cor, a largura e a dinâmica do Tlender. **De outros programas:** um OBJ com curvas (`l`, polilinhas: Blender exporta curvas assim) vira um ator **Hair** com o Groom. No Details, **Guides** escolhe as guias (ou Nenhuma: crescido na cabeça); cada fio segue as 3 guias mais perto.
- **Muitos personagens** (uma multidão com cabelo em fios, como o GTA 6): a engine divide um orçamento de fios por quadro entre todos pelo tamanho na tela, e só os **16 mais perto simulam** (os outros ficam como cresceram e voltam a simular sem salto ao chegar perto). 100 cabeças com cabelo dinâmico: 257 fps (todos simulando: 125). `REALENGINE_HAIR_NO_BUDGET=1` compara.
- **Ainda não:** o cabelo não faz sombra na pele.
- Para comparar sem: `REALENGINE_NO_HAIR=1`. Cenas de teste: `hair`, `hair_dynamic`.
--- en
```diagram steps
Add the Groom component to the head's actor
The strands grow over the top of the mesh, combed back
Gravity, curls and clumps give the shape
Each strand is lit as a fiber (two highlights, light coming through)
```
The **Groom** (like Unreal's Groom Component) grows hair in **real strands** over the actor's mesh, instead of textured cards.
- **How to use:** select the head's actor, **+ Add > Groom**. Pick a **Preset** (Short, Long, Wavy, Curly) and adjust: **Strands** (how many), **Length**, **Curl** and **Curl Frequency**, **Gravity** (how much it falls), **Clumping** (locks), **Scalp** (how far down the head it grows) and **Hairline** (the forehead: the actor looks along +X).
- **Color:** **Melanin** (0 light blond ... 1 black) and **Redness** (red hair), as in Unreal; **Dye** tints over it; **Roughness** widens the shine.
- **Light:** a fiber's three highlights (the white one, the colored one below it and the light coming through against the sun) and the shadow the hair casts on itself (a density grid).
- **Far away:** with **LOD** on, fewer strands, wider: the same look for less work.
- **Cost** (RTX 3060, Epic): ~0.3-0.7 ms with 20 thousand strands on a head near the camera.
- **Dynamic (physics):** turn on **Dynamic** and the hair is simulated on the GPU: it falls, swings when the head moves, the wind moves it and it **collides** with the head, the character's bones (Skeletal Mesh: shoulders, back, arms; with a Physics Asset, by its bodies' radii) and the scene's boxes. **Stiffness** holds the style (low: loose), **Damping** calms it, **Wind** (cm/s²) and **Gusts** give the wind, **Collision Radius** the volume. Off, it stays as grown, for free.
- **From the Tlender:** import the `.tlend` (File > Import): each strand hair becomes a `.rgroom` in `Imported/Hair` and a Groom on the surface's actor, with the Tlender's count, color, width and dynamics. **From other programs:** an OBJ with curves (`l`, polylines: Blender exports curves this way) becomes a **Hair** actor with the Groom. In Details, **Guides** picks the guides (or None: grown on the head); each strand follows its 3 nearest guides.
- **Many characters** (a crowd with hair in strands, like GTA 6): the engine shares a strand budget a frame among all of them by their size on the screen, and only the **16 nearest simulate** (the rest stay as grown and simulate again without a jump when they come near). 100 heads with dynamic hair: 257 fps (all simulating: 125). `REALENGINE_HAIR_NO_BUDGET=1` compares.
- **Not yet:** the hair casts no shadow on the skin.
- To compare without it: `REALENGINE_NO_HAIR=1`. Test scenes: `hair`, `hair_dynamic`.
