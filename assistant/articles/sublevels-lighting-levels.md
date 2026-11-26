id: sublevels-lighting-levels
kind: recipe
title: Subníveis e Lighting Levels empilháveis | Sublevels and stackable Lighting Levels | Subniveles y Lighting Levels
keys: subnivel, subniveis, sublevel, sublevels, painel levels, levels panel, janela niveis, window levels, nivel persistente, persistent level, nivel atual, current level, make current, tornar atual, mover atores para o nivel, move actors to level, travar nivel, lock level, carregado no inicio, initially loaded, adicionar existente, add existing, criar novo nivel, create new level, lighting level, lighting levels, light level, cenario de iluminacao, lighting scenario, varios lighting levels, multiple lighting scenarios, iluminacao somada, added lighting, dia e noite, day and night, luzes da rua, street lights, build lighting por nivel, set lighting level visible, is lighting level visible, set lighting level blend, get lighting level blend, mistura, blend, peso da luz, fade de luz, luz piscando, flicker, timeline de luz, apagar devagar, acender devagar, load stream level, rlevels
--- pt
**Janela > Níveis** é o painel Levels da Unreal: o **Nível Persistente** (o arquivo aberto) e os **subníveis**, outros níveis do projeto carregados no mesmo mundo, cada um salvo no próprio `.rlevel` (bom para dividir o trabalho: a arte num, a jogabilidade noutro, a iluminação noutro).
- **Adicionar Existente...** escolhe um nível do projeto; **Criar Novo...** cria um vazio ao lado e já o torna o atual.
- Cada nível é um **cartão**: o **olho** (mostra/esconde no editor), o tipo (sol = **Lighting**, dados = nível comum), o nome (`*` = alterado), os atores, **No início** (Carregado no Início), o cadeado (**Travar**: não seleciona nem move), a cor e a lixeira (Remover, com confirmação e Ctrl+Z). Clique num cartão e o Organizador mostra só os atores dele. Clique duas vezes num nível para torná-lo o **atual**: os atores novos (colocados, arrastados, colados) vão para ele. Botão direito: Tornar Atual, Mover Atores Selecionados para o Nível, Selecionar Atores, Salvar Nível, Remover.
- **Salvar** (Ctrl+S) grava o persistente sem os atores dos subníveis, cada subnível alterado no arquivo dele e a lista `<nível>.rlevels`.
- **Play e jogo**: entram só os subníveis **Carregados no Início**; os outros entram por **Load Stream Level** com o nome deles (e saem por Unload Stream Level).
- **Lighting Levels** (o diferencial): marque **Lighting** num subnível e as luzes dele viram uma iluminação baked própria. O **Build Lighting** faz um bake do nível persistente e depois um de cada Lighting Level sozinho; a iluminação mostrada é a **soma** dos que estão visíveis. Na Unreal só um Lighting Scenario fica ligado por vez; aqui quantos quiser ao mesmo tempo (o dia + os postes da rua + o letreiro da loja), porque a luz é linear e a soma dos bakes é exata.
- **Grafo**: **Set Lighting Level Visible** (nome, ligado) liga ou desliga um Lighting Level no jogo — as luzes e a parte baked dele — e **Is Lighting Level Visible** diz se está ligado.
- **Mistura (Blend)**: cada Lighting Level tem um peso (0 desligado, 1 como no bake, acima de 1 mais forte), no painel ao lado de Lighting e no nó **Set Lighting Level Blend**. As luzes e a luz baked seguem na hora, na GPU: ligue a uma **Timeline** para acender devagar, fazer um letreiro piscar ou um relâmpago clarear. **Get Lighting Level Blend** lê o peso.
- Mudou malhas Static de lugar? Faça o Build Lighting de novo: um bake feito com outras malhas fica de fora da soma (o Output Log avisa).

```diagram steps
Janela > Níveis | Criar Novo... "Lighting_Noite" | marque Lighting | coloque as luzes nele | Build > Build Lighting | o olho liga e desliga a noite
```
```blueprint
Event Begin Play -> Set Lighting Level Visible ( Level Name = "Lighting_Noite", Visible = false )
Event Key N -> Set Lighting Level Visible ( Level Name = "Lighting_Noite", Visible = true )
Event Key L -> Set Lighting Level Visible ( Level Name = "Lighting_Postes", Visible = true )
```
--- en
**Window > Levels** is Unreal's Levels panel: the **Persistent Level** (the file you opened) and its **sublevels**, other levels of the project loaded into the same world, each saved to its own `.rlevel` (good for splitting the work: art in one, gameplay in another, lighting in another).
- **Add Existing...** picks a level of the project; **Create New...** makes an empty one beside it and makes it the current one.
- Each level is a **card**: the **eye** (show/hide in the editor), its kind (sun = **Lighting**, data = a plain level), its name (`*` = changed), its actors, **At start** (Initially Loaded), the padlock (**Lock**: not selected or moved), its color and the trash can (Remove, asked first, Ctrl+Z). Click a card and the Outliner shows only its actors. Double-click a level to make it **current**: new actors (placed, dragged, pasted) go into it. Right-click: Make Current, Move Selected Actors to Level, Select Actors, Save Level, Remove.
- **Save** (Ctrl+S) writes the persistent level without the sublevels' actors, each changed sublevel to its own file and the list `<level>.rlevels`.
- **Play and the game**: only the **Initially Loaded** sublevels come in; the others come in with **Load Stream Level** by their name (and leave with Unload Stream Level).
- **Lighting Levels** (the difference): tick **Lighting** on a sublevel and its lights become a baked lighting of their own. **Build Lighting** bakes the persistent level, then each Lighting Level alone; the lighting shown is the **sum** of the visible ones. In Unreal only one Lighting Scenario is on at a time; here as many as you like at once (daylight + the street lamps + the shop sign), because light is linear and the sum of the bakes is exact.
- **Graph**: **Set Lighting Level Visible** (name, on) turns a Lighting Level on or off in the game — its lights and its baked share — and **Is Lighting Level Visible** tells whether it is on.
- **Blend**: each Lighting Level has a weight (0 off, 1 as baked, above 1 brighter), in the panel beside Lighting and in the **Set Lighting Level Blend** node. Its lights and its baked light follow at once, on the GPU: drive it from a **Timeline** to fade lamps in, make a sign flicker or lightning flash. **Get Lighting Level Blend** reads it.
- Moved Static meshes? Build Lighting again: a bake made with other meshes is left out of the sum (the Output Log says so).

```diagram steps
Window > Levels | Create New... "Lighting_Night" | tick Lighting | put its lights in it | Build > Build Lighting | the eye turns the night on and off
```
```blueprint
Event Begin Play -> Set Lighting Level Visible ( Level Name = "Lighting_Night", Visible = false )
Event Key N -> Set Lighting Level Visible ( Level Name = "Lighting_Night", Visible = true )
Event Key L -> Set Lighting Level Visible ( Level Name = "Lighting_StreetLamps", Visible = true )
```
--- es
**Ventana > Levels** es el panel Levels de Unreal: el **Persistent Level** (el archivo que abriste) y sus **subniveles**, otros niveles del proyecto cargados en el mismo mundo, cada uno guardado en su propio `.rlevel` (bueno para dividir el trabajo: arte en uno, gameplay en otro, iluminación en otro).
- **Add Existing...** elige un nivel del proyecto; **Create New...** crea uno vacío a su lado y lo convierte en el actual.
- Cada nivel es una **tarjeta**: el **ojo** (mostrar/ocultar en el editor), su tipo (sol = **Lighting**, dato = un nivel simple), su nombre (`*` = modificado), sus actores, **At start** (Initially Loaded), el candado (**Lock**: no se selecciona ni se mueve), su color y el cubo de basura (Remove, con confirmación previa, Ctrl+Z). Haz clic en una tarjeta y el Organizador muestra solo sus actores. Doble clic en un nivel para hacerlo el **actual**: los actores nuevos (colocados, arrastrados, pegados) van a él. Clic derecho: Make Current, Move Selected Actors to Level, Select Actors, Save Level, Remove.
- **Guardar** (Ctrl+S) escribe el nivel persistente sin los actores de los subniveles, cada subnivel modificado en su propio archivo y la lista `<nivel>.rlevels`.
- **Jugar y el juego**: solo entran los subniveles **Initially Loaded**; los demás entran con **Load Stream Level** por su nombre (y salen con Unload Stream Level).
- **Lighting Levels** (la diferencia): marca **Lighting** en un subnivel y sus luces pasan a ser una iluminación precalculada propia. **Build Lighting** precalcula el nivel persistente y luego cada Lighting Level por separado; la iluminación mostrada es la **suma** de los visibles. En Unreal solo hay un Lighting Scenario activo a la vez; aquí tantos como quieras a la vez (luz del día + las farolas + el letrero de la tienda), porque la luz es lineal y la suma de los bakes es exacta.
- **Grafo**: **Set Lighting Level Visible** (nombre, activado) enciende o apaga un Lighting Level en el juego —sus luces y su parte precalculada— y **Is Lighting Level Visible** dice si está encendido.
- **Blend**: cada Lighting Level tiene un peso (0 apagado, 1 como se precalculó, por encima de 1 más brillante), en el panel junto a Lighting y en el nodo **Set Lighting Level Blend**. Sus luces y su luz precalculada lo siguen al instante, en la GPU: contrólalo desde un **Timeline** para encender lámparas gradualmente, hacer parpadear un letrero o un relámpago. **Get Lighting Level Blend** lo lee.
- ¿Moviste Static meshes? Vuelve a hacer Build Lighting: un bake hecho con otras mallas se deja fuera de la suma (el Output Log lo avisa).

```diagram steps
Ventana > Levels | Create New... "Lighting_Night" | marca Lighting | pon sus luces en él | Build > Build Lighting | el ojo enciende y apaga la noche
```
```blueprint
Event Begin Play -> Set Lighting Level Visible ( Level Name = "Lighting_Night", Visible = false )
Event Key N -> Set Lighting Level Visible ( Level Name = "Lighting_Night", Visible = true )
Event Key L -> Set Lighting Level Visible ( Level Name = "Lighting_StreetLamps", Visible = true )
```
