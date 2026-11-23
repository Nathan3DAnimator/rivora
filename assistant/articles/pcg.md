id: pcg
kind: howto
title: PCG: geração procedural de conteúdo (florestas, pedras, cercas por grafo) | PCG: procedural content generation (forests, rocks, fences by a graph)
keys: pcg, procedural, procedural content generation, geracao procedural, generacion procedural, conteudo procedural, espalhar, scatter, dispersar, floresta, forest, bosque, arvores, trees, arboles, pedras, rocks, rocas, piedras, cerca, fence, valla, grafo pcg, pcg graph, volume pcg, pcg volume, pontos, points, puntos, surface sampler, amostrador de superficie, volume sampler, spline sampler, ao longo da spline, along the spline, filtro de densidade, density filter, inclinacao, steepness, slope, camada do landscape, landscape layer, self pruning, auto exclusao, sobreposicao, overlap, transform points, static mesh spawner, spawn actor, nested pcg, pcg aninhado, subgrafo, subgraph, parametros, parameters, semente, seed, set pcg seed, generate pcg, cleanup pcg, set pcg parameter, world partition, celula, cell, particionado, partitioned, deterministico, deterministic, mapa procedural, procedural map, vegetacao, vegetation, foliage procedural
--- pt
```diagram steps
Adicionar > Mundo > Volume PCG (a caixa onde ele gera)
Details > PCG > Graph: PCG_Forest (ou o seu, Gaveta > Adicionar > PCG)
Surface Sampler -> Spatial Noise -> Density Filter -> Steepness Filter
Self Pruning -> Transform Points -> Static Mesh Spawner
Semente: o mesmo resultado sempre; Particionado: por célula do World Partition
```
```blueprint
Event BeginPlay
  -> Set PCG Seed ( Target = PCGVolume, Seed = 1234 )
  -> Set PCG Parameter ( Target = PCGVolume, Name = "Tree Density", Value = 0.08 )
  -> Generate PCG ( Target = PCGVolume )
```
O **PCG** espalha conteúdo por um **grafo**: pontos feitos no chão, num volume ou ao longo de splines, **filtrados**, **movidos** e **criados** como malhas, atores ou outros grafos. É o PCG da Unreal, mais simples.
1. **Coloque um Volume PCG** (Adicionar > Mundo > Volume PCG). A caixa dele (**Extensão do Volume** nos Detalhes) é onde ele gera; ele já vem com o exemplo **PCG_Forest**. Com **Gerar ao Mudar**, o editor refaz quando você muda o grafo, a semente, a caixa, as malhas e splines dentro dela ou esculpe o chão.
2. **O grafo** (Gaveta de Conteúdo > Adicionar > PCG, clique duas vezes para editar): arraste de uma saída a uma entrada para ligar; botão direito adiciona nós. **Fontes**: Surface Sampler (Landscape e malhas), Volume Sampler, Spline Sampler (Trilhos, Rios, Spline Components). **Filtros**: Attribute, Density, Steepness, Landscape Layer (as camadas pintadas). **Pontos**: Density Noise, Spatial Noise (manchas e clareiras), Create Attribute, Transform Points (posição, rotação e escala aleatórias), Project to Surface, **Self Pruning** (tira os que se sobrepõem), Difference, Merge. **Criadores**: Static Mesh Spawner (por peso ou todas as entradas: tronco e copa), Spawn Actor (uma classe Blueprint), **Nested PCG** (outro grafo em volta de cada ponto) e **Subgraph**.
3. **Parâmetros**: no painel do grafo, **+ Parâmetro**; o botão **P** ao lado de um número o liga a um parâmetro. Um Volume PCG troca esses números nos Detalhes, e um Blueprint com **Set PCG Parameter**. Parâmetros de mesmo nome passam para os Subgrafos e PCGs aninhados.
4. **Depuração**: **D** num nó (ou Detalhes > Mostrar Pontos) mostra os pontos na viewport, coloridos pela densidade; cada nó mostra quantos pontos fez e quanto custou.
5. **Em jogo**: **Gerar ao Carregar** faz quando o nível abre; **Gerar Sob Demanda** espera o **Generate PCG**. **Particionado** gera por **célula do World Partition** em volta do jogador, nos workers do job system, e solta as células longe. A **semente** decide tudo: a mesma semente faz a mesma floresta em qualquer máquina; **Set PCG Seed** troca a semente (um mapa procedural por partida). **Cleanup PCG** remove o que ele fez.
- Exemplos em **PCG/Examples**: **PCG_Forest** (árvores em manchas, arbustos longe delas, parâmetro Tree Density), **PCG_Rocks** (pedras viradas, algumas com musgo por atributo), **PCG_RockClusters** (grupos de pedras: um PCG aninhado em cada lugar) e **PCG_Fence** (cerca ao longo de uma spline).
- Custo (RTX 3060): 100 mil pontos do Volume Sampler em ~1 ms nos workers; um grafo inteiro com ruído, transformação, auto-exclusão e criador em ~25 ms; uma célula de 128 m de floresta em ~0,3 ms; o pior quadro do PCG jogando abaixo de 1,5 ms. O que ele cria nunca é salvo no nível: é refeito pela semente.
- Veja também: [Landscape](article:landscape), [Foliage](article:foliage), [World Partition](article:world-partition).
--- en
```diagram steps
Add > World > PCG Volume (the box it generates in)
Details > PCG > Graph: PCG_Forest (or yours, Content Drawer > Add > PCG)
Surface Sampler -> Spatial Noise -> Density Filter -> Steepness Filter
Self Pruning -> Transform Points -> Static Mesh Spawner
Seed: the same result every time; Partitioned: by World Partition cell
```
```blueprint
Event BeginPlay
  -> Set PCG Seed ( Target = PCGVolume, Seed = 1234 )
  -> Set PCG Parameter ( Target = PCGVolume, Name = "Tree Density", Value = 0.08 )
  -> Generate PCG ( Target = PCGVolume )
```
**PCG** scatters content by a **graph**: points made on the ground, in a volume or along splines, **filtered**, **moved** and **spawned** as meshes, actors or other graphs. It is Unreal's PCG, simpler.
1. **Place a PCG Volume** (Add > World > PCG Volume). Its box (**Volume Extent** in Details) is where it generates; it starts with the **PCG_Forest** example. With **Generate On Change**, the editor makes it again when you change the graph, the seed, the box, the meshes and splines in it, or sculpt the ground.
2. **The graph** (Content Drawer > Add > PCG, double-click to edit): drag from an output to an input to wire; right click adds nodes. **Sources**: Surface Sampler (Landscape and meshes), Volume Sampler, Spline Sampler (Rails, Rivers, Spline Components). **Filters**: Attribute, Density, Steepness, Landscape Layer (the painted layers). **Points**: Density Noise, Spatial Noise (patches and clearings), Create Attribute, Transform Points (random location, rotation and scale), Project to Surface, **Self Pruning** (removes the overlapping ones), Difference, Merge. **Spawners**: Static Mesh Spawner (by weight or every entry: trunk and crown), Spawn Actor (a Blueprint class), **Nested PCG** (another graph around each point) and **Subgraph**.
3. **Parameters**: in the graph's panel, **+ Parameter**; the **P** button beside a number binds it to one. Each PCG Volume sets its own value in Details, and a Blueprint with **Set PCG Parameter**. Parameters of the same name pass down to Subgraphs and nested PCGs.
4. **Debugging**: **D** on a node (or Details > Show Points) shows its points in the viewport, colored by density; each node shows how many points it made and what it cost.
5. **In game**: **Generate on Load** makes it when the level opens; **Generate on Demand** waits for **Generate PCG**. **Partitioned** generates by **World Partition cell** around the player, on the job system's workers, and drops the far cells. The **seed** decides everything: the same seed makes the same forest on any machine; **Set PCG Seed** changes it (a procedural map per run). **Cleanup PCG** removes what it made.
- Examples in **PCG/Examples**: **PCG_Forest** (trees in patches, bushes kept off them, a Tree Density parameter), **PCG_Rocks** (rocks turned every way, some mossy by an attribute), **PCG_RockClusters** (rock groups: a nested PCG at each spot) and **PCG_Fence** (a fence along a spline).
- Cost (RTX 3060): 100 000 Volume Sampler points in ~1 ms on the workers; a whole graph with noise, transform, self pruning and a spawner in ~25 ms; a 128 m cell of forest in ~0.3 ms; the PCG's worst frame while playing under 1.5 ms. What it spawns is never saved in the level: it is made again from the seed.
- See also: [Landscape](article:landscape), [Foliage](article:foliage), [World Partition](article:world-partition).
--- es
```diagram steps
Añadir > Mundo > Volumen PCG (la caja donde genera)
Detalles > PCG > Graph: PCG_Forest (o el tuyo, Cajón de Contenido > Añadir > PCG)
Surface Sampler -> Spatial Noise -> Density Filter -> Steepness Filter
Self Pruning -> Transform Points -> Static Mesh Spawner
Semilla: siempre el mismo resultado; Particionado: por celda del World Partition
```
```blueprint
Event BeginPlay
  -> Set PCG Seed ( Target = PCGVolume, Seed = 1234 )
  -> Set PCG Parameter ( Target = PCGVolume, Name = "Tree Density", Value = 0.08 )
  -> Generate PCG ( Target = PCGVolume )
```
El **PCG** reparte contenido con un **grafo**: puntos hechos en el suelo, en un volumen o a lo largo de splines, **filtrados**, **movidos** y **creados** como mallas, actores u otros grafos. Es el PCG de Unreal, más simple.
1. **Coloca un Volumen PCG** (Añadir > Mundo > Volumen PCG). Su caja (**Extensión del Volumen** en Detalles) es donde genera; empieza con el ejemplo **PCG_Forest**. Con **Generar al Cambiar**, el editor lo rehace cuando cambias el grafo, la semilla, la caja, las mallas y splines dentro de ella o esculpes el suelo.
2. **El grafo** (Cajón de Contenido > Añadir > PCG, doble clic para editar): arrastra de una salida a una entrada para conectar; clic derecho añade nodos. **Fuentes**: Surface Sampler (Landscape y mallas), Volume Sampler, Spline Sampler (Rieles, Ríos, Spline Components). **Filtros**: Attribute, Density, Steepness, Landscape Layer (las capas pintadas). **Puntos**: Density Noise, Spatial Noise (manchas y claros), Create Attribute, Transform Points (posición, rotación y escala aleatorias), Project to Surface, **Self Pruning** (quita los que se superponen), Difference, Merge. **Creadores**: Static Mesh Spawner (por peso o todas las entradas: tronco y copa), Spawn Actor (una clase Blueprint), **Nested PCG** (otro grafo alrededor de cada punto) y **Subgraph**.
3. **Parámetros**: en el panel del grafo, **+ Parámetro**; el botón **P** junto a un número lo conecta a uno. Cada Volumen PCG cambia su valor en Detalles, y un Blueprint con **Set PCG Parameter**. Los parámetros del mismo nombre pasan a los Subgrafos y PCG anidados.
4. **Depuración**: **D** en un nodo (o Detalles > Mostrar Puntos) muestra sus puntos en la viewport, coloreados por la densidad; cada nodo muestra cuántos puntos hizo y cuánto costó.
5. **En el juego**: **Generar al Cargar** lo hace cuando abre el nivel; **Generar Bajo Demanda** espera el **Generate PCG**. **Particionado** genera por **celda del World Partition** alrededor del jugador, en los workers del job system, y suelta las celdas lejanas. La **semilla** lo decide todo: la misma semilla hace el mismo bosque en cualquier máquina; **Set PCG Seed** la cambia (un mapa procedural por partida). **Cleanup PCG** quita lo que hizo.
- Ejemplos en **PCG/Examples**: **PCG_Forest** (árboles en manchas, arbustos lejos de ellos, parámetro Tree Density), **PCG_Rocks** (rocas giradas, algunas con musgo por atributo), **PCG_RockClusters** (grupos de rocas: un PCG anidado en cada lugar) y **PCG_Fence** (una valla a lo largo de una spline).
- Costo (RTX 3060): 100 000 puntos del Volume Sampler en ~1 ms en los workers; un grafo entero con ruido, transformación, auto-exclusión y creador en ~25 ms; una celda de 128 m de bosque en ~0,3 ms; el peor cuadro del PCG jugando por debajo de 1,5 ms. Lo que crea nunca se guarda en el nivel: se rehace con la semilla.
- Ver también: [Landscape](article:landscape), [Foliage](article:foliage), [World Partition](article:world-partition).
