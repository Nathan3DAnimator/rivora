id: blueprints-basics
kind: howto
title: Grafos: o básico | Graphs: the basics | Grafos: lo básico
keys: blueprint, nos, programacao visual, evento, begin play, tick, variavel, funcao, classe, grafo, visual scripting, pino de asset, asset pin, sequencia por variavel, som por variavel, trocar sequencia, modular, variavel no lugar do valor, tipo da variavel, trocar tipo, variable type, expose on spawn, expor ao criar, olho, instance editable, editavel por instancia, referencia de asset, asset reference, variavel de level sequence, variavel de animacao, variavel de som
--- pt
```diagram steps
Adicionar > Blueprint Class | Escolher o pai | Event Graph: eventos e nós | Compile | Arrastar para o nível
```
1. Navegador de Conteúdo: **Adicionar > Classe de Grafo** e escolha o pai (Actor, Character, Pawn...).
2. Duplo clique abre o editor (como o da Unreal): Viewport, Construction Script e o **Event Graph**.
3. Clique com o botão direito no grafo para adicionar nós; ligue os pinos brancos (execução) a partir de um evento: **Event BeginPlay** (ao começar) ou **Event Tick** (todo quadro).
4. Variáveis ficam no painel da esquerda; marque **Instance Editable** para editá-las no Details de cada ator.
5. **Compile** e arraste a classe para o nível.

Prefere código? Veja [Scripts](article:scripts-basics).
- **Tudo é pino:** o asset ou nome que um nó usa (a **Sequence** do Play Sequence, o **Sound** do Play Sound, o **System** das partículas, o **Widget Name**, a **Key Name** do Blackboard, o **Montage**...) é também um **pino de entrada** do tipo Name: o valor do Details aparece nele, e ligando uma variável (Name ou String) o nó usa o asset que ela tiver na hora. O delegate do Custom Event fica no **título** do nó, como na Unreal.

## Variáveis (como na Unreal)
- Na lista **Variáveis** do painel Meu Grafo, cada variável mostra o **tipo numa pílula colorida**: clique nela para trocar o tipo ali mesmo. O **olho** liga e desliga o **Editável por Instância** (aberto: cada ator no nível pode ter o seu valor no Detalhes).
- O seletor de tipos é organizado: **Busca**, **Básicos**, **Estruturas**, **Enumerações**, **Referências de Asset** (Level Sequence, Animação, Montage, Som, Material, Textura, Static Mesh, Skeletal Mesh, Sistema de Partículas, Behavior Tree, Data Asset, Diálogo, Missão, Input Action, Nível, Veículo...) e as referências de objeto e de classe dos seus Grafos.
- Uma **referência de asset** guarda o asset pelo nome, com o padrão escolhido na lista dos assets do projeto daquele tipo, e liga direto no pino do asset dos nós (o **Sequence** do Play Sequence, o **Animation** do Play Animation, o **Sound** do Play Sound...).
- **Expose on Spawn** (Detalhes da variável): a variável vira um pino do **Spawn Actor from Class** daquela classe; o valor dado ali chega antes do Construction Script e do BeginPlay do ator novo.
--- en
```diagram steps
Add > Blueprint Class | Pick the parent | Event Graph: events and nodes | Compile | Drag into the level
```
1. Content Browser: **Add > Graph Class** and pick the parent (Actor, Character, Pawn...).
2. Double-click opens its editor (like Unreal's): Viewport, Construction Script and the **Event Graph**.
3. Right-click the graph to add nodes; wire the white pins (execution) from an event: **Event BeginPlay** (at the start) or **Event Tick** (every frame).
4. Variables are on the left panel; check **Instance Editable** to edit them in each actor's Details.
5. **Compile** and drag the class into the level.

Prefer code? See [Scripts](article:scripts-basics).
- **Everything is a pin:** the asset or name a node uses (Play Sequence's **Sequence**, Play Sound's **Sound**, the particles' **System**, the **Widget Name**, the Blackboard's **Key Name**, the **Montage**...) is also a Name **input pin**: the Details value shows in it, and wiring a variable (Name or String) makes the node use whatever asset it holds then. The Custom Event's delegate sits in the node's **title**, as in Unreal.

## Variables (as in Unreal)
- In My Graph's **Variables** list each variable shows its **type in a colored pill**: click it to change the type right there. The **eye** turns **Instance Editable** on and off (open: each actor in the level can have its own value in Details).
- The type picker is organized: **Search**, **Basic**, **Structures**, **Enumerations**, **Asset References** (Level Sequence, Animation, Montage, Sound, Material, Texture, Static Mesh, Skeletal Mesh, Particle System, Behavior Tree, Data Asset, Dialogue, Quest, Input Action, Level, Vehicle...) and your Graphs' object and class references.
- An **asset reference** keeps the asset by name, its default picked from the project's assets of that kind, and wires straight into the nodes' asset pin (Play Sequence's **Sequence**, Play Animation's **Animation**, Play Sound's **Sound**...).
- **Expose on Spawn** (the variable's Details): the variable becomes a pin of **Spawn Actor from Class** for that class; the value given there arrives before the new actor's Construction Script and BeginPlay.
--- es
```diagram steps
Añadir > Clase Blueprint | Elige el padre | Event Graph: eventos y nodos | Compilar | Arrástrala al nivel
```
1. Explorador de contenidos: **Añadir > Clase de Grafo** y elige el padre (Actor, Character, Pawn...).
2. Doble clic abre su editor (como el de Unreal): Ventana gráfica, Construction Script y el **Event Graph**.
3. Clic derecho en el grafo para añadir nodos; conecta los pines blancos (ejecución) desde un evento: **Event BeginPlay** (al inicio) o **Event Tick** (cada cuadro).
4. Las variables están en el panel izquierdo; marca **Instance Editable** para editarlas en los Detalles de cada actor.
5. **Compilar** y arrastra la clase al nivel.

¿Prefieres código? Mira [Scripts](article:scripts-basics).
- **Todo es un pin:** el asset o nombre que usa un nodo (el **Sequence** de Play Sequence, el **Sound** de Play Sound, el **System** de las partículas, el **Widget Name**, el **Key Name** del Blackboard, el **Montage**...) es también un **pin de entrada** de tipo Name: el valor de Detalles aparece en él, y al conectar una variable (Name o String) el nodo usa entonces el asset que ella contenga. El delegado del Custom Event está en el **título** del nodo, como en Unreal.

## Variables (como en Unreal)
- En la lista **Variables** de My Graph cada variable muestra su **tipo en una píldora de color**: haz clic para cambiar el tipo allí mismo. El **ojo** activa y desactiva **Instance Editable** (abierto: cada actor del nivel puede tener su propio valor en Detalles).
- El selector de tipo está organizado: **Búsqueda**, **Básicos**, **Estructuras**, **Enumeraciones**, **Referencias de asset** (Level Sequence, Animation, Montage, Sound, Material, Texture, Static Mesh, Skeletal Mesh, Particle System, Behavior Tree, Data Asset, Dialogue, Quest, Input Action, Level, Vehicle...) y las referencias de objeto y de clase de tus Grafos.
- Una **referencia de asset** guarda el asset por nombre, con su valor por defecto elegido entre los assets del proyecto de ese tipo, y se conecta directo al pin de asset de los nodos (el **Sequence** de Play Sequence, el **Animation** de Play Animation, el **Sound** de Play Sound...).
- **Expose on Spawn** (en los Detalles de la variable): la variable se convierte en un pin de **Spawn Actor from Class** para esa clase; el valor dado allí llega antes del Construction Script y del BeginPlay del nuevo actor.
