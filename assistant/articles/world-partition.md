id: world-partition
kind: concept
title: Mundos grandes (World Partition, Data Layers, HLOD) | Big worlds (World Partition, Data Layers, HLOD) | World Partition
keys: mundo aberto grande, carregando por distancia, carregar por distância, streaming por distancia, open world streaming, world partition, mundo aberto grande, carregando por distancia, carregar por distancia, carregar o mapa por partes, mapa por partes, camadas de atores, ligar e desligar camadas, ligar camada, desligar camada, mostrar e esconder grupos de atores no jogo, actor layers, toggle layers at runtime, mundo grande, mundos grandes, mundo aberto, open world, big world, large world, big level, nivel grande, mapa grande, streaming, level streaming, streaming de nivel, carregar por distancia, load by distance, grade, grid, cell, celula, celulas, loading range, alcance de carga, data layer, data layers, camada de dados, camadas, hlod, lod de longe, far lod, distant meshes, malhas distantes, merged mesh, malha combinada, build hlods, construir hlods, is spatially loaded, carregado espacialmente, set data layer runtime state, is location loaded, engasgo, travada, hitch, stutter, 4 km, mil atores, thousands of actors
--- pt
```diagram steps
Adicionar > Landscape > World Partition (um por nível)
Os atores entram na grade de células (128 m) sozinhos
Compilar > Construir HLODs: o que está longe vira uma malha só
Play: as células perto da câmera entram, as de longe saem, sem engasgo
```
O **World Partition** deixa um nível enorme rodar como um pequeno: só o que está perto do jogador fica no mundo (como o da Unreal). É o jeito de fazer um **mundo aberto grande** que vai **carregando por distância**.
- **Como ligar:** **Adicionar > Landscape > World Partition**. Nos Details dele: **Cell Size** (128 m), **Loading Range** (256 m: as células até essa distância da câmera ficam no mundo) e **Streaming Budget** (ms por quadro para carregar e descarregar: o resto espera o próximo quadro, os mais perto primeiro).
- **Quem entra na grade:** cada ator tem **World Partition > Is Spatially Loaded**. **Auto** põe na grade os props, luzes locais, pickups, inimigos e Grafos; o sol, o céu, a neblina, o Landscape, o jogador, as regras do jogo e os volumes grandes ficam sempre. **No** prende um ator no mundo (o chão de uma peça só, por exemplo).
- **Ao voltar:** um ator que saiu volta como o nível o tinha (o reset da Unreal). Para guardar o que mudou (um baú aberto, um inimigo morto), use o componente **Salvável**.
- **Data Layers:** nos Details do World Partition, **+ Data Layer** cria uma camada com nome e **estado inicial**: **Unloaded** (fora), **Loaded** (dentro, escondida) ou **Activated** (dentro e visível). Marque a camada nos Details de cada ator. No jogo, **Set Data Layer Runtime State** troca o estado: o interior de uma masmorra, a cidade antes e depois da batalha, a decoração de um evento. O olho ao lado do nome esconde a camada no editor.
- **HLOD (o que se vê de longe):** perto do limite, os atores que saíram continuam visíveis como **proxies** (a mesma malha, sem Grafo, sem colisão). Longe, **Compilar > Construir HLODs** junta cada região de 4 x 4 células numa malha só, simplificada (**HLOD Triangles**, 5 %) e com os materiais assados nas cores dos vértices (`Content/HLOD/<nível>/`). Malhas menores que **HLOD Min Size** ficam de fora.
- **Em alta velocidade:** as células do caminho dos próximos 2 s vão carregando antes, pela distância até você chegar.
- **Is Location Loaded:** verdadeiro quando a célula de um ponto está no mundo: espere por ele antes de teleportar ou criar algo lá.
- **O mapa:** os Details do World Partition mostram as células (mais claras: mais atores) e o círculo do Loading Range; no Play, as verdes estão no mundo e as azuis aparecem pelo HLOD.
- **Custo** (RTX 3060, cena `wp`: 4 x 4 km, 6000 atores, atravessada a 400 m/s): 252 fps contra 240 fps com tudo carregado, 0,05 ms por quadro de streaming (pior 0,26 ms) e 0,16 % de quadros acima do dobro da mediana, contra 3,2 % sem ele.
- **Cuidado:** um ator da grade não deve guardar referência a outro de outra célula (ele pode não estar lá), como na Unreal.
```blueprint
Event BeginPlay -> Set Data Layer Runtime State ( Data Layer = "Dungeon", State = 2 )
Event Key E -> Branch ( Is Location Loaded ( Location = Target ) ) -> Set Actor Location ( New Location = Target )
```
```real
class Portao extends Actor
    var Destino: Vector

    event BeginPlay()
        SetDataLayerRuntimeState("Masmorra", 2)

    event Tick(dt: float)
        if IsLocationLoaded(Destino)
            print("o destino ja esta no mundo")
```
--- en
```diagram steps
Add > Landscape > World Partition (one per level)
The actors join the grid of cells (128 m) by themselves
Build > Build HLODs: what is far becomes a single mesh
Play: the cells near the camera come in, the far ones leave, no hitch
```
**World Partition** lets a huge level run like a small one: only what is near the player stays in the world (like Unreal's).
- **How to turn it on:** **Add > Landscape > World Partition**. In its Details: **Cell Size** (128 m), **Loading Range** (256 m: the cells this close to the camera stay in the world) and **Streaming Budget** (ms a frame for loading and unloading: the rest waits for the next frame, the nearest first).
- **Who joins the grid:** each actor has **World Partition > Is Spatially Loaded**. **Auto** puts props, local lights, pickups, enemies and Graphs on the grid; the sun, the sky, the fog, the Landscape, the player, the game's rules and big volumes always stay. **No** keeps an actor in the world (a one-piece floor, for instance).
- **When it comes back:** an actor that left returns as the level had it (Unreal's reset). To keep what changed (an opened chest, a dead enemy), use the **Saveable** component.
- **Data Layers:** in the World Partition's Details, **+ Data Layer** makes a named layer with an **initial state**: **Unloaded** (out), **Loaded** (in, hidden) or **Activated** (in and shown). Tick the layer in each actor's Details. In the game, **Set Data Layer Runtime State** changes it: a dungeon's interior, the town before and after the battle, an event's decorations. The eye next to the name hides the layer in the editor.
- **HLOD (what you see from far):** near the edge, the actors that left stay visible as **proxies** (the same mesh, no Graph, no collision). Far away, **Build > Build HLODs** merges each region of 4 x 4 cells into one mesh, simplified (**HLOD Triangles**, 5 %) with the materials baked into vertex colors (`Content/HLOD/<level>/`). Meshes smaller than **HLOD Min Size** are left out.
- **At high speed:** the cells on your next 2 s of path load early, by the distance until you get there.
- **Is Location Loaded:** true when a point's cell is in the world: wait for it before teleporting or spawning something there.
- **The map:** the World Partition's Details show the cells (lighter: more actors) and the Loading Range's circle; in Play, the green ones are in the world and the blue ones show through their HLOD.
- **Cost** (RTX 3060, scene `wp`: 4 x 4 km, 6000 actors, crossed at 400 m/s): 252 fps against 240 fps with everything loaded, 0.05 ms a frame of streaming (worst 0.26 ms) and 0.16 % of frames over twice the median, against 3.2 % without it.
- **Careful:** an actor on the grid shouldn't keep a reference to one in another cell (it may not be there), as in Unreal.
```blueprint
Event BeginPlay -> Set Data Layer Runtime State ( Data Layer = "Dungeon", State = 2 )
Event Key E -> Branch ( Is Location Loaded ( Location = Target ) ) -> Set Actor Location ( New Location = Target )
```
```real
class Gate extends Actor
    var Destination: Vector

    event BeginPlay()
        SetDataLayerRuntimeState("Dungeon", 2)

    event Tick(dt: float)
        if IsLocationLoaded(Destination)
            print("the destination is in the world")
```
--- es
```diagram steps
Añadir > Landscape > World Partition (uno por nivel)
Los actores entran solos en la cuadrícula de celdas (128 m)
Compilar > Build HLODs: lo lejano se convierte en una sola malla
Jugar: las celdas cercanas a la cámara entran, las lejanas salen, sin tirones
```
**World Partition** permite que un nivel enorme corra como uno pequeño: solo lo que está cerca del jugador permanece en el mundo (como en Unreal).
- **Cómo activarlo:** **Añadir > Landscape > World Partition**. En sus Detalles: **Cell Size** (128 m), **Loading Range** (256 m: las celdas a esta distancia de la cámara permanecen en el mundo) y **Streaming Budget** (ms por cuadro para cargar y descargar: el resto espera al siguiente cuadro, las más cercanas primero).
- **Quién entra en la cuadrícula:** cada actor tiene **World Partition > Is Spatially Loaded**. **Auto** pone en la cuadrícula props, luces locales, pickups, enemigos y Grafos; el sol, el cielo, la niebla, el Landscape, el jugador, las reglas del juego y los volúmenes grandes siempre se quedan. **No** mantiene un actor en el mundo (un suelo de una sola pieza, por ejemplo).
- **Cuando vuelve:** un actor que salió regresa como lo tenía el nivel (el reinicio de Unreal). Para conservar lo que cambió (un cofre abierto, un enemigo muerto), usa el componente **Saveable**.
- **Data Layers:** en los Detalles del World Partition, **+ Data Layer** crea una capa con nombre y un **estado inicial**: **Unloaded** (fuera), **Loaded** (dentro, oculta) o **Activated** (dentro y visible). Marca la capa en los Detalles de cada actor. En el juego, **Set Data Layer Runtime State** la cambia: el interior de un calabozo, el pueblo antes y después de la batalla, las decoraciones de un evento. El ojo junto al nombre oculta la capa en el editor.
- **HLOD (lo que se ve desde lejos):** cerca del borde, los actores que salieron siguen visibles como **proxies** (la misma malla, sin Grafo, sin colisión). Lejos, **Compilar > Build HLODs** fusiona cada región de 4 x 4 celdas en una malla simplificada (**HLOD Triangles**, 5 %) con los materiales horneados en colores de vértice (`Content/HLOD/<nivel>/`). Las mallas más pequeñas que **HLOD Min Size** se dejan fuera.
- **A alta velocidad:** las celdas de tu trayectoria de los próximos 2 s se cargan antes, según la distancia hasta llegar a ellas.
- **Is Location Loaded:** verdadero cuando la celda de un punto está en el mundo: espéralo antes de teletransportar o generar algo allí.
- **El mapa:** los Detalles del World Partition muestran las celdas (más claras: más actores) y el círculo del Loading Range; en Jugar, las verdes están en el mundo y las azules se ven a través de su HLOD.
- **Costo** (RTX 3060, escena `wp`: 4 x 4 km, 6000 actores, cruzada a 400 m/s): 252 fps contra 240 fps con todo cargado, 0,05 ms por cuadro de streaming (peor caso 0,26 ms) y 0,16 % de los cuadros por encima del doble de la mediana, contra 3,2 % sin él.
- **Cuidado:** un actor de la cuadrícula no debería guardar una referencia a uno de otra celda (puede no estar allí), como en Unreal.
```blueprint
Event BeginPlay -> Set Data Layer Runtime State ( Data Layer = "Dungeon", State = 2 )
Event Key E -> Branch ( Is Location Loaded ( Location = Target ) ) -> Set Actor Location ( New Location = Target )
```
```real
class Gate extends Actor
    var Destination: Vector

    event BeginPlay()
        SetDataLayerRuntimeState("Dungeon", 2)

    event Tick(dt: float)
        if IsLocationLoaded(Destination)
            print("the destination is in the world")
```
