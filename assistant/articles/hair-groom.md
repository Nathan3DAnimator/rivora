id: hair-groom
kind: concept
title: Cabelo de verdade em fios (Groom) | Hair in strands (Groom) | Cabello en hebras (Groom)
keys: cabelo de verdade em fios, cabelo em fios no personagem, pelo na skeletal mesh, fur on skeletal mesh, pelo do tlender, tlender fur, groom visible, cabelo segue os ossos, cabelo, hair, fios, strands, groom, groom component, componente groom, cabelo realista, realistic hair, pelo, fur, barba, beard, marschner, melanina, melanin, ruivo, red hair, loiro, blond, cacheado, curly, ondulado, wavy, cabelo comprido, long hair, hair cards, cards de cabelo, sombra do cabelo, hair shadow, REALENGINE_NO_HAIR, cabelo dinâmico, dynamic hair, física do cabelo, hair physics, cabelo balançando, hair swinging, vento no cabelo, wind in hair, colisão do cabelo, hair collision, importar cabelo do tlender, import tlender hair, guias, guides, rgroom, curvas obj, obj curves, polilinhas
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
- **Pelo do Tlender numa Skeletal Mesh:** um hair cuja superfície é deformada por uma armadura (modificador Armature ou presa a um osso) vai para o ator **SK_** da importação, não para um ator solto. Cada fio segue os **ossos da raiz** (os 4 ossos e pesos do vértice de pele mais perto): o pelo acompanha a animação. O cabelo **Dynamic** acompanha o osso principal das raízes (a cabeça) e simula a partir dele.
- **Ligar e desligar:** a caixa **Visible** do Groom (no jogo: **Set Property by Name** "Visible"); desligado, nem desenha nem simula. Um ator escondido no jogo também não mostra o cabelo.
- **Seleção:** o cabelo do ator selecionado entra no contorno laranja junto com o ator.
- **Por Blueprint ou script:** **Set Groom Visible**, **Set Groom Simulate** (estático: segue só a pele; simulado: balança, cai e colide) e os **Get** de cada um, no ator ou no componente de cabelo dele. Com um **Physics Asset**, o cabelo colide com os corpos dele no lugar da elipse da cabeça.
- **Simulação por guias:** acima de 2000 fios, só 1 em 8 é simulado (os líderes); os outros seguem o líder mais perto: o custo cai a um oitavo, como as guias da Unreal.
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
- **The Tlender's fur on a Skeletal Mesh:** a hair whose surface an armature bends (an Armature modifier, or riding a bone) goes on the import's **SK_** actor, not on an actor of its own. Each strand follows its **root's bones** (the nearest skin vertex's 4 bones and weights): the fur moves with the animation. **Dynamic** hair follows the roots' main bone (the head) and simulates from it.
- **On and off:** the Groom's **Visible** box (in the game: **Set Property by Name** "Visible"); off, it is neither drawn nor simulated. An actor hidden in the game hides its hair too.
- **Selection:** the selected actor's hair joins its orange outline.
- **From Blueprints or scripts:** **Set Groom Visible**, **Set Groom Simulate** (static: it follows the skin only; simulated: it swings, falls and collides) and the **Get** of each, on the actor or its hair component. With a **Physics Asset**, the hair collides with its bodies instead of the head's ellipsoid.
- **Simulated by guides:** above 2000 strands only 1 in 8 is simulated (the leaders); the rest follow their nearest leader: an eighth of the cost, like Unreal's guides.
- **Not yet:** the hair casts no shadow on the skin.
- To compare without it: `REALENGINE_NO_HAIR=1`. Test scenes: `hair`, `hair_dynamic`.
--- es
```diagram steps
Añade el componente Groom al actor de la cabeza
Las hebras crecen sobre la malla, peinadas hacia atrás
La gravedad, los rizos y los mechones dan la forma
Cada hebra se ilumina como una fibra (dos brillos, luz que la atraviesa)
```
El **Groom** (como el Groom Component de Unreal) hace crecer el cabello en **hebras reales** sobre la malla del actor, en lugar de tarjetas con textura.
- **Cómo usarlo:** selecciona el actor de la cabeza, **+ Añadir > Groom**. Elige un **Preset** (Corto, Largo, Ondulado, Rizado) y ajusta: **Strands** (cuántas), **Length**, **Curl** y **Curl Frequency**, **Gravity** (cuánto cae), **Clumping** (mechones), **Scalp** (hasta dónde baja en la cabeza) y **Hairline** (la frente: el actor mira hacia +X).
- **Color:** **Melanin** (0 rubio claro ... 1 negro) y **Redness** (cabello rojo), como en Unreal; **Dye** tiñe encima; **Roughness** ensancha el brillo.
- **Luz:** los tres brillos de una fibra (el blanco, el de color debajo y la luz que la atraviesa contra el sol) y la sombra que el cabello proyecta sobre sí mismo (una cuadrícula de densidad).
- **A lo lejos:** con **LOD** activado, menos hebras y más anchas: el mismo aspecto con menos trabajo.
- **Costo** (RTX 3060, Épico): ~0,3-0,7 ms con 20 mil hebras en una cabeza cerca de la cámara.
- **Dinámico (física):** activa **Dynamic** y el cabello se simula en la GPU: cae, se balancea cuando la cabeza se mueve, el viento lo mueve y **colisiona** con la cabeza, los huesos del personaje (Skeletal Mesh: hombros, espalda, brazos; con un Physics Asset, por los radios de sus cuerpos) y las cajas de la escena. **Stiffness** sostiene el peinado (bajo: suelto), **Damping** lo calma, **Wind** (cm/s²) y **Gusts** dan el viento, **Collision Radius** el volumen. Desactivado, queda como creció, sin costo.
- **Desde Tlender:** importa el `.tlend` (Archivo > Importar): cada cabello de hebras se convierte en un `.rgroom` en `Imported/Hair` y en un Groom en el actor de la superficie, con la cantidad, el color, el ancho y la dinámica de Tlender. **Desde otros programas:** un OBJ con curvas (`l`, polilíneas: Blender exporta las curvas así) se convierte en un actor **Hair** con el Groom. En Detalles, **Guides** elige las guías (o None: crece sobre la cabeza); cada hebra sigue a sus 3 guías más cercanas.
- **Muchos personajes** (una multitud con cabello en hebras, como GTA 6): el motor reparte un presupuesto de hebras por cuadro entre todos según su tamaño en pantalla, y solo los **16 más cercanos simulan** (los demás quedan como crecieron y vuelven a simular sin un salto al acercarse). 100 cabezas con cabello dinámico: 257 fps (todas simulando: 125). `REALENGINE_HAIR_NO_BUDGET=1` compara.
- **El pelo de Tlender en una Skeletal Mesh:** un cabello cuya superficie deforma un armature (modificador Armature, o pegada a un hueso) va al actor **SK_** de la importación, no a un actor suelto. Cada hebra sigue los **huesos de su raíz** (los 4 huesos y pesos del vértice de piel más cercano): el pelo acompaña la animación. El cabello **Dynamic** sigue el hueso principal de las raíces (la cabeza) y simula a partir de él.
- **Activar y desactivar:** la casilla **Visible** del Groom (en el juego: **Set Property by Name** "Visible"); desactivado, ni se dibuja ni se simula. Un actor oculto en el juego tampoco muestra el cabello.
- **Selección:** el cabello del actor seleccionado entra en su contorno naranja.
- **Desde Blueprints o scripts:** **Set Groom Visible**, **Set Groom Simulate** (estático: sigue solo la piel; simulado: se balancea, cae y colisiona) y el **Get** de cada uno, en el actor o en su componente de cabello. Con un **Physics Asset**, el cabello colisiona con sus cuerpos en lugar de la elipse de la cabeza.
- **Simulado por guías:** por encima de 2000 hebras solo 1 de cada 8 se simula (las líderes); las demás siguen a su líder más cercana: un octavo del costo, como las guías de Unreal.
- **Aún no:** el cabello no proyecta sombra sobre la piel.
- Para comparar sin él: `REALENGINE_NO_HAIR=1`. Escenas de prueba: `hair`, `hair_dynamic`.
