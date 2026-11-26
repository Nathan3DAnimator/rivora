id: blueprint-communication
kind: howto
title: Um Grafo falar com outro (Cast, interfaces, dispatchers) | One Graph talking to another (Cast, interfaces, dispatchers) | Un Grafo que habla con otro (Cast, interfaces, dispatchers)
keys: delegate, quadrado vermelho, red square, bind event, output delegate, set timer by event, comunicacao entre blueprints, custom event com parametros, evento com entrada, call custom event, chamar evento de outro ator, chamar outro blueprint, cast to, interface, event dispatcher, referencia de outro ator, falar com outro ator, get all actors of class, pegar referencia, grafo, grafos, graph, graphs
--- pt
- **Cast To**: "este ator é da classe X?" Se for, você acessa as variáveis e funções dele.
- **Interface**: um contrato de funções que várias classes implementam (Interagir, Levar Dano). Chame sem saber a classe.
- **Event Dispatcher**: um ator avisa ("morri"), quem estiver ligado (Bind) reage. Como na Unreal: ligue o **quadradinho vermelho** (o delegate) de um **Custom Event** na entrada **Event** do **Bind Event to**; quando o dispatcher for chamado, esse evento roda. O mesmo quadrado serve no **Set Timer by Event** e no **Unbind Event from**.
- **Custom Event com entradas** (Details do evento > Inputs > + Add): chame com **Call Custom Event**; em outro ator, escolha **Target Class** no Details da chamada e ligue o **Target** (como chamar o evento de um ator que você pegou no Cast).
- Para achar atores: **Get Player Character**, **Get All Actors of Class**, o ator do overlap ou uma variável Actor preenchida no Details.
--- en
- **Cast To**: "is this actor of class X?" If it is, you reach its variables and functions.
- **Interface**: a contract of functions many classes implement (Interact, Take Damage). Call it without knowing the class.
- **Event Dispatcher**: an actor announces ("I died"), whoever is bound (Bind) reacts. As in Unreal: wire a **Custom Event**'s **little red square** (its delegate) into **Bind Event to**'s **Event** input; when the dispatcher is called, that event runs. The same square goes into **Set Timer by Event** and **Unbind Event from**.
- **Custom Event with inputs** (the event's Details > Inputs > + Add): call it with **Call Custom Event**; on another actor, pick **Target Class** in the call's Details and wire the **Target** (like calling the event of an actor you got from a Cast).
- To find actors: **Get Player Character**, **Get All Actors of Class**, the overlap's actor or an Actor variable filled in Details.
--- es
- **Cast To**: "¿este actor es de la clase X?" Si lo es, accedes a sus variables y funciones.
- **Interface**: un contrato de funciones que muchas clases implementan (Interact, Take Damage). Se llama sin conocer la clase.
- **Event Dispatcher**: un actor anuncia ("morí"), y quien esté enlazado (Bind) reacciona. Como en Unreal: conecta el **cuadradito rojo** de un **Custom Event** (su delegado) a la entrada **Event** de **Bind Event to**; cuando se llama al dispatcher, ese evento se ejecuta. El mismo cuadradito va a **Set Timer by Event** y a **Unbind Event from**.
- **Custom Event con entradas** (Detalles del evento > Inputs > + Añadir): se llama con **Call Custom Event**; en otro actor, elige **Target Class** en los Detalles de la llamada y conecta el **Target** (como al llamar el evento de un actor obtenido de un Cast).
- Para encontrar actores: **Get Player Character**, **Get All Actors of Class**, el actor del overlap o una variable Actor rellenada en Detalles.
