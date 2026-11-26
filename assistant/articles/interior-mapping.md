id: interior-mapping
kind: howto
title: Interiores pela janela (Interior Mapping) | Rooms behind windows (Interior Mapping) | Interior Mapping: salas detrás de las ventanas
keys: interior mapping, interiores, interior, parallax de interiores, interior parallax, parallax interior, salas, rooms, habitaciones, janelas, windows, ventanas, predio, building, edificio, fachada, facade, cidade, city, ciudad, interior cubemap, atlas de interiores, interior atlas, moveis, furniture, muebles, cortinas, curtains, luz noturna, night lights, luces nocturnas, janelas acesas, lit windows, ventanas encendidas, M_Building, REALENGINE_NO_INTERIOR
--- pt
```diagram steps
Principled BSDF (a fachada)
Interior Mapping (salas, profundidade, móveis, cortinas, noite)
Material Output (Surface)
Value "Night" com Parameter Name: o Blueprint acende a cidade
```
O nó **Interior Mapping** (Shader > Interiores) põe uma **sala atrás de cada janela** sem nenhuma geometria: a sala tem profundidade de verdade e muda com o ângulo da câmera (como o Interior Cubemap da Unreal e os prédios de jogos de mundo aberto).
1. Ligue-o entre o **Principled BSDF** (a fachada: concreto, tijolo) e o **Material Output**.
2. **Rooms U / Rooms V**: quantas janelas no UV 0..1 da face. **Room Depth**: a profundidade (em larguras de sala). **Frame**: a moldura (a fachada) em volta de cada janela.
3. **Furniture** (sofá, mesa, estante em camada no meio da sala), **Curtains** (cortinas atrás do vidro, com dobras, fechadas por sala ao acaso).
4. **Noite**: **Night** (0 dia, 1 noite), **Lit Rooms** (a parte das salas com a luz acesa, sorteada por janela; algumas trocam a cada meio minuto), **Light Color** e **Light Strength**. Ligue um **Value** com **Parameter Name** "Night" no pino para mudar por Material Instance ou Grafo sem recompilar.
5. **Atlas** (opcional): uma imagem com várias salas (fotos ou renders de cada sala vista da janela) e as **Colunas/Linhas do atlas**; sem imagem as salas são geradas (paredes coloridas, piso de madeira, teto claro). Exemplo pronto: **M_BuildingAtlas** com **T_RoomsAtlas** (4 × 4 salas) em Materials/Examples.
- De dia a sala recebe uns 5% da luz de fora (céu e sol), como uma sala de verdade; à noite as lâmpadas aparecem quando a exposição é a da noite.
```blueprint
Event BeginPlay
  -> Set Scalar Parameter Value on Materials
       Night
       Target = self, Parameter Value = 1.0
```
- A janela vira vidro (escuro e liso: reflete o céu) e a sala aparece através dele.
- Cena de teste: `interiors` (quatro prédios, dois de dia e dois de noite).
- Custo (RTX 3060, 1440p Épico, quatro fachadas cobrindo a tela): +0,46 ms (2,86 → 3,32 ms). `REALENGINE_NO_INTERIOR=1` para comparar. Zero para quem não usa.
- Veja também: [Parallax](article:parallax), [Materiais](article:materials).
--- en
```diagram steps
Principled BSDF (the facade)
Interior Mapping (rooms, depth, furniture, curtains, night)
Material Output (Surface)
A "Night" Value with a Parameter Name: a Blueprint lights the city
```
The **Interior Mapping** node (Shader > Interiors) puts a **room behind every window** with no geometry at all: the room has real depth and changes with the camera's angle (like Unreal's Interior Cubemap and open world games' buildings).
1. Put it between the **Principled BSDF** (the facade: concrete, brick) and the **Material Output**.
2. **Rooms U / Rooms V**: how many windows across the face's UV 0..1. **Room Depth**: the depth (in room widths). **Frame**: the frame (the facade) around each window.
3. **Furniture** (a sofa, a desk, a shelf as a layer in the middle of the room), **Curtains** (behind the glass, with folds, drawn by a random amount per room).
4. **Night**: **Night** (0 day, 1 night), **Lit Rooms** (the share of rooms with the light on, drawn per window; some switch every half minute), **Light Color** and **Light Strength**. Link a **Value** with the **Parameter Name** "Night" to the pin to change it by Material Instance or Graph without a recompile.
5. **Atlas** (optional): an image of several rooms (photos or renders of each room seen from the window) and the **atlas Columns/Rows**; without an image the rooms are made up (colored walls, a wooden floor, a light ceiling). Ready example: **M_BuildingAtlas** with **T_RoomsAtlas** (4 × 4 rooms) in Materials/Examples.
- By day a room gets about 5% of the light outside (sky and sun), as a real room; at night the lamps show when the exposure is a night's.
```blueprint
Event BeginPlay
  -> Set Scalar Parameter Value on Materials
       Night
       Target = self, Parameter Value = 1.0
```
- The window turns glass (dark and smooth: it reflects the sky) and the room shows through it.
- Test scene: `interiors` (four buildings, two by day and two at night).
- Cost (RTX 3060, 1440p Epic, four facades filling the screen): +0.46 ms (2.86 → 3.32 ms). `REALENGINE_NO_INTERIOR=1` to compare. Nothing for whoever doesn't use it.
- See also: [Parallax](article:parallax), [Materials](article:materials).
--- es
```diagram steps
Principled BSDF (la fachada)
Interior Mapping (salas, profundidad, muebles, cortinas, noche)
Material Output (Surface)
Un Value "Night" con un Parameter Name: un Blueprint ilumina la ciudad
```
El nodo **Interior Mapping** (Shader > Interiors) pone una **sala detrás de cada ventana** sin nada de geometría: la sala tiene profundidad real y cambia con el ángulo de la cámara (como el Interior Cubemap de Unreal y los edificios de los juegos de mundo abierto).
1. Colócalo entre el **Principled BSDF** (la fachada: hormigón, ladrillo) y el **Material Output**.
2. **Rooms U / Rooms V**: cuántas ventanas hay a lo ancho del UV 0..1 de la cara. **Room Depth**: la profundidad (en anchos de sala). **Frame**: el marco (la fachada) alrededor de cada ventana.
3. **Furniture** (un sofá, un escritorio, una estantería como una capa en medio de la sala), **Curtains** (detrás del cristal, con pliegues, corridas en una cantidad aleatoria por sala).
4. **Night**: **Night** (0 día, 1 noche), **Lit Rooms** (la proporción de salas con la luz encendida, sorteada por ventana; algunas cambian cada medio minuto), **Light Color** y **Light Strength**. Conecta un **Value** con el **Parameter Name** "Night" al pin para cambiarlo con una Material Instance o un Grafo sin recompilar.
5. **Atlas** (opcional): una imagen de varias salas (fotos o renders de cada sala vista desde la ventana) y las **Columns/Rows del atlas**; sin imagen las salas se inventan (paredes de colores, un suelo de madera, un techo claro). Ejemplo listo: **M_BuildingAtlas** con **T_RoomsAtlas** (4 × 4 salas) en Materials/Examples.
- De día una sala recibe cerca del 5% de la luz de afuera (cielo y sol), como una sala real; de noche las lámparas se ven cuando la exposición es la de una noche.
```blueprint
Event BeginPlay
  -> Set Scalar Parameter Value on Materials
       Night
       Target = self, Parameter Value = 1.0
```
- La ventana se vuelve cristal (oscuro y liso: refleja el cielo) y la sala se ve a través de ella.
- Escena de prueba: `interiors` (cuatro edificios, dos de día y dos de noche).
- Coste (RTX 3060, 1440p Épico, cuatro fachadas llenando la pantalla): +0.46 ms (2.86 → 3.32 ms). `REALENGINE_NO_INTERIOR=1` para comparar. Nada para quien no lo usa.
- Mira también: [Parallax](article:parallax), [Materiales](article:materials).
