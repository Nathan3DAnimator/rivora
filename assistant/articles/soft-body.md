id: soft-body
kind: howto
title: Corpo mole: almofadas, colchões, bolas de borracha, gelatina, barriga | Soft body: cushions, mattresses, rubber balls, jelly, belly | Cuerpo blando: cojines, colchones, pelotas de goma, gelatina, barriga
keys: corpo mole, soft body, soft physics, fisica mole, almofada, cushion, sofa, sofá, colchao, mattress, espuma, foam, borracha, rubber, bola, ball, gelatina, jelly, gordura, fat, barriga, belly, carne, flesh, jiggle, amassar, squash, deformar, deform, volume, tetraedros, tetrahedra, neo-hookean, vbd, plasticidade, plasticity, memory foam, espuma viscoelastica, amassado, dent, preso, held, pinned, soltar, release, criar corpo mole a partir da selecao, make soft body from selected, chaos flesh
--- pt
```diagram steps
Selecione uma Static Mesh (almofada, colchão, bola) | Ator > Criar Corpo Mole a partir da Seleção (ou Adicionar Rápido > Corpo Mole) | Detalhes > Corpo Mole: Predefinição, Rigidez, Manter Volume, Preso Por | Jogar (Alt+P): amassa, volta, segura caixas | Blueprint: Add Soft Body Impulse, Set Soft Body Stiffness, Set Soft Body Pinned
```
## Como funciona
Um **Corpo Mole** é um ator com a sua **Static Mesh** e um componente **Cloth** do tipo **Corpo Mole**. A malha é colocada dentro de uma **rede de cubos** (as células que ela preenche), cada cubo dividido em **6 tetraedros**, simulada na GPU pelo mesmo **Vertex Block Descent** do tecido com elasticidade **Neo-Hookean estável**: amassa, volta e **mantém o volume** (Manter Volume perto de 0,5 estufa dos lados quando apertado). A malha é desenhada dobrada com o **seu próprio material**, sombras e raios.
- **Predefinições**: Espuma, Borracha, Gelatina, Almofada, Carne (densidade, rigidez, volume, amortecimento e amassado de uma vez).
- **Preso Por**: Livre (cai e rola), Baixo (a almofada no assento), Faixa, Osso (barriga e gordura presas a um osso com **Seguir Ator/Osso**, sumindo ao longo da **Transição**) ou **Pintado** (as cores de vértice da malha: vermelho preso, preto livre).
- **Plasticidade**: o amassado fica e volta devagar (**Volta do Amassado**), como espuma viscoelástica.
- **Colide** com as malhas do nível, os personagens (os ossos), os corpos com física, os tecidos e os outros corpos moles. Uma **caixa com física** cai na almofada, **afunda e para**: o corpo mole empurra de volta (**Empurrar Corpos Físicos**).
- **Dorme** parado (custo zero) ou longe da câmera, e acorda quando tocado. O sofá de 3 almofadas custa ~0,47 ms numa RTX 3060.
- Use uma malha com vértices suficientes (uma caixa de 24 vértices só dobra nos cantos): as malhas importadas e a esfera servem.
```blueprint
Event Hit -> Add Soft Body Impulse ( Target = self, Location = Hit Location, Radius = 30, Velocity = Make Vector(0, 0, -300) )
Event Key H -> Set Soft Body Stiffness ( Target = self, Stiffness = 40 )
Event Key R -> Set Soft Body Pinned ( Target = self, Pinned = false )
```
--- en
```diagram steps
Select a Static Mesh (cushion, mattress, ball) | Actor > Make Soft Body from Selected (or Quick Add > Soft Body) | Details > Soft Body: Preset, Stiffness, Keep Volume, Held By | Play (Alt+P): it squashes, springs back, holds boxes | Blueprint: Add Soft Body Impulse, Set Soft Body Stiffness, Set Soft Body Pinned
```
## How it works
A **Soft Body** is an actor with its **Static Mesh** and a **Cloth** component of kind **Soft Body**. The mesh is put inside a **lattice of cubes** (the cells it fills), each cube split in **6 tetrahedra**, simulated on the GPU by the cloth's own **Vertex Block Descent** with **stable Neo-Hookean** elasticity: it squashes, springs back and **keeps its volume** (Keep Volume near 0.5 bulges at the sides when squeezed). The mesh is drawn bent with **its own material**, shadows and rays.
- **Presets**: Foam, Rubber, Jelly, Cushion, Flesh (density, stiffness, volume, damping and dents at once).
- **Held By**: Free (it falls and rolls), Bottom (a cushion on its seat), Band, Bone (belly and fat held to a bone with **Follow Actor/Bone**, fading over the **Falloff**) or **Painted** (the mesh's vertex colors: red held, black free).
- **Plasticity**: a dent stays and comes back slowly (**Dent Recovery**), like memory foam.
- It **collides** with the level's meshes, the characters (their bones), physics bodies, cloths and the other soft bodies. A **physics box** dropped on a cushion **sinks in and stops**: the soft body pushes back (**Push Physics Bodies**).
- It **sleeps** when still (no cost) or far from the camera, and wakes when touched. The sofa of 3 cushions costs ~0.47 ms on an RTX 3060.
- Use a mesh with enough vertices (a 24-vertex box only bends at its corners): imported meshes and the sphere do.
```blueprint
Event Hit -> Add Soft Body Impulse ( Target = self, Location = Hit Location, Radius = 30, Velocity = Make Vector(0, 0, -300) )
Event Key H -> Set Soft Body Stiffness ( Target = self, Stiffness = 40 )
Event Key R -> Set Soft Body Pinned ( Target = self, Pinned = false )
```
--- es
```diagram steps
Selecciona una Static Mesh (cojín, colchón, pelota) | Actor > Make Soft Body from Selected (o Quick Add > Soft Body) | Detalles > Soft Body: Preset, Stiffness, Keep Volume, Held By | Jugar (Alt+P): se aplasta, rebota, sostiene cajas | Blueprint: Add Soft Body Impulse, Set Soft Body Stiffness, Set Soft Body Pinned
```
## Cómo funciona
Un **Soft Body** es un actor con su **Static Mesh** y un componente **Cloth** de tipo **Soft Body**. La malla se coloca dentro de una **red de cubos** (las celdas que llena), cada cubo dividido en **6 tetraedros**, simulada en la GPU por el propio **Vertex Block Descent** de la tela con elasticidad **Neo-Hookean estable**: se aplasta, rebota y **conserva su volumen** (Keep Volume cerca de 0,5 se abulta por los lados al apretarlo). La malla se dibuja doblada con **su propio material**, sombras y rayos.
- **Presets**: Foam, Rubber, Jelly, Cushion, Flesh (densidad, rigidez, volumen, amortiguación y abolladuras de una vez).
- **Held By**: Free (cae y rueda), Bottom (un cojín sobre su asiento), Band, Bone (barriga y grasa sujetas a un hueso con **Follow Actor/Bone**, desvaneciéndose a lo largo del **Falloff**) o **Painted** (los colores de vértice de la malla: rojo sujeto, negro libre).
- **Plasticidad**: una abolladura se queda y vuelve despacio (**Dent Recovery**), como la espuma viscoelástica.
- **Colisiona** con las mallas del nivel, los personajes (sus huesos), los cuerpos de física, las telas y los demás cuerpos blandos. Una **caja de física** soltada sobre un cojín **se hunde y se detiene**: el cuerpo blando empuja de vuelta (**Push Physics Bodies**).
- **Duerme** cuando está quieto (sin costo) o lejos de la cámara, y despierta al tocarlo. El sofá de 3 cojines cuesta ~0,47 ms en una RTX 3060.
- Usa una malla con suficientes vértices (una caja de 24 vértices solo se dobla en sus esquinas): las mallas importadas y la esfera sí lo tienen.
```blueprint
Event Hit -> Add Soft Body Impulse ( Target = self, Location = Hit Location, Radius = 30, Velocity = Make Vector(0, 0, -300) )
Event Key H -> Set Soft Body Stiffness ( Target = self, Stiffness = 40 )
Event Key R -> Set Soft Body Pinned ( Target = self, Pinned = false )
```
