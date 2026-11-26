id: graph-menu
kind: howto
title: Menu do botão direito nos grafos | The right-click menu in graphs | Menú de nodos de los grafos (recientes y recomendados)
keys: estilo dos nos, estilo dos fios, graph style, node style, wire style, neon, vidro, circuito, arrastar variavel, arrastar funcao, arrastar componente, drag variable, drag function, drag component, get component, categorias, caixas, botao direito, right click, menu de nos, node menu, all actions, todas as acoes, recentes, recent, recomendados, recommended, sugestao de no, sugestões, historico, history, context sensitive, sensivel ao contexto, adicionar no, add node, blueprint, animgraph, material, behavior tree, sound cue, particulas, tamanho do menu, menu gigante, menu pequeno, zoom do menu, busca de material, buscar no material, material search, grafo, grafos, graph, graphs
--- pt
```diagram steps
Botão direito no grafo (ou solte um fio no vazio) | Recentes: as 2 últimas que você usou | Recomendados: 2 que conectam com o que você está fazendo | Depois, só as ações que este grafo aceita
```
## Como funciona
Em todo grafo (Grafo, Função, Macro, Construction Script, AnimGraph, Material, Behavior Tree, Sound Cue e Sistema de Partículas), o **botão direito** abre o menu de nós:
- **Recentes**: as **2 últimas** ações que você usou nesse tipo de grafo (guardadas entre sessões).
- **Recomendados**: **2 ações** que costumam vir depois do que você está fazendo: o nó do fio que você puxou, o nó selecionado ou o último que você colocou. Ex.: depois de **Event BeginPlay**, *Print String* e *Delay*; depois de **Get Actor Location**, *Set Actor Location*.
- O menu **aprende**: o que você coloca depois de um nó passa a ser recomendado depois dele.
- **Só o que o grafo aceita**: numa **função** não aparecem eventos nem nós latentes (Delay); no **Construction Script** também não; os nós de **Animation Graph** só num Animation Graph. Puxando um fio, só o que encaixa nele.
- Digitar na busca mostra todas as ações do grafo que batem com o texto.
- As ações ficam em **caixas por categoria que abrem e fecham** (Flow Control, Utilities > Time...), como na Unreal; buscando, as caixas abrem sozinhas com só o que bate.
- **Arrastar do My Grafo:** variáveis (Ctrl Get, Alt Set), **funções** (vira a chamada), **macros** e, do painel Components, um **componente** (vira *Get <componente>*).
- **Estilo dos grafos:** em **Editar > Preferências do Editor > Estilo dos Grafos**, escolha o estilo dos nós (Padrão, Plano, Minimalista, Vidro, Neon) e, separado, o dos fios (Curva, Reto, Circuito, Brilho, Fluxo), cada um por uma prévia; muda na hora.
- O menu do botão direito tem **sempre o mesmo tamanho**, qualquer que seja o zoom do grafo.
- **Material:** o menu de nós do material tem **busca**: digite logo ao abrir ("tex img" acha Image Texture), Enter cria o primeiro.
--- en
```diagram steps
Right click the graph (or drop a wire on empty space) | Recent: the last 2 you used | Recommended: 2 that connect with what you are doing | Then only the actions this graph takes
```
## How it works
In every graph (Graph, Function, Macro, Construction Script, AnimGraph, Material, Behavior Tree, Sound Cue and Particle System), the **right button** opens the node menu:
- **Recent**: the **last 2** actions you used in that kind of graph (kept between sessions).
- **Recommended**: **2 actions** that usually come after what you are doing: the node of the wire you dragged, the selected node or the last one you placed. E.g. after **Event BeginPlay**, *Print String* and *Delay*; after **Get Actor Location**, *Set Actor Location*.
- The menu **learns**: what you place after a node becomes recommended after it.
- **Only what the graph takes**: a **function** shows no events and no latent nodes (Delay); neither does the **Construction Script**; **Animation Graph** nodes only in one. Dragging a wire, only what fits it.
- Typing in the search shows every action of the graph that matches.
- Actions sit in **category boxes that open and close** (Flow Control, Utilities > Time...), as in Unreal; searching opens the boxes with only the matches.
- **Drag from My Graph:** variables (Ctrl Get, Alt Set), **functions** (a call), **macros** and, from the Components panel, a **component** (a *Get <component>*).
- **Graph style:** in **Edit > Editor Preferences > Graph Style**, pick the nodes' style (Default, Flat, Minimal, Glass, Neon) and, apart, the wires' (Curve, Straight, Circuit, Glow, Flow), each from a preview; it changes at once.
- The right-click menu is **always the same size**, whatever the graph's zoom.
- **Material:** the material's node menu has a **search**: type as it opens ("tex img" finds Image Texture), Enter makes the first one.
--- es
```diagram steps
Clic derecho en el grafo (o suelta un cable en el vacío) | Recientes: los últimos 2 que usaste | Recomendados: 2 que conectan con lo que haces | Luego solo las acciones que este grafo admite
```
## Cómo funciona
En todo grafo (Graph, Function, Macro, Construction Script, AnimGraph, Material, Behavior Tree, Sound Cue y Particle System), el **botón derecho** abre el menú de nodos:
- **Recientes**: las **últimas 2** acciones que usaste en ese tipo de grafo (se guardan entre sesiones).
- **Recomendados**: **2 acciones** que suelen venir después de lo que haces: el nodo del cable que arrastraste, el nodo seleccionado o el último que colocaste. Ej.: después de **Event BeginPlay**, *Print String* y *Delay*; después de **Get Actor Location**, *Set Actor Location*.
- El menú **aprende**: lo que colocas después de un nodo pasa a ser recomendado tras él.
- **Solo lo que el grafo admite**: una **función** no muestra eventos ni nodos latentes (Delay); el **Construction Script** tampoco; los nodos de **Animation Graph** solo en uno. Al arrastrar un cable, solo lo que encaja.
- Al escribir en la búsqueda se muestran todas las acciones del grafo que coinciden.
- Las acciones están en **cajas de categoría que se abren y se cierran** (Flow Control, Utilities > Time...), como en Unreal; al buscar, se abren las cajas que tienen coincidencias.
- **Arrastrar desde My Graph:** variables (Ctrl Get, Alt Set), **funciones** (una llamada), **macros** y, desde el panel de Componentes, un **componente** (un *Get <componente>*).
- **Estilo de grafo:** en **Editar > Preferencias del editor > Graph Style**, elige el estilo de los nodos (Default, Flat, Minimal, Glass, Neon) y, aparte, el de los cables (Curve, Straight, Circuit, Glow, Flow), cada uno con una vista previa; cambia al instante.
- El menú del clic derecho tiene **siempre el mismo tamaño**, sea cual sea el zoom del grafo.
- **Material:** el menú de nodos del material tiene una **búsqueda**: escribe al abrirlo ("tex img" encuentra Image Texture), Enter crea el primero.
