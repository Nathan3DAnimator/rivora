id: hud
kind: recipe
title: HUD do jogo: barra de vida, bússola, mensagens, marcadores, mira, minimapa | Game HUD: health bar, compass, messages, markers, crosshair, minimap | HUD modular (barras, brújula, mensajes, números de daño, mira, marcadores, minimapa)
keys: hud, interface do jogo, tela do jogo, game hud, hud layout, layout do hud, wbp_gamehud, barra de vida, health bar, barra de escudo, shield bar, barra de stamina, stamina bar, mana, barra de atributo, attribute bar, bind attribute bar, ligar barra, barra do chefe, boss bar, barra do inimigo, enemy bar, trecho perdido, lost chunk, dano atrasado, delayed damage, bussola, bússola, compass, pontos cardeais, norte, north, marcador, marker, marcador na bussola, compass marker, add compass marker, remove compass marker, marcador na tela, screen marker, add screen marker, remove screen marker, objetivo, objective, waypoint, seta na borda, off screen, fora da tela, indicador de direcao, mensagem, mensagens, message feed, feed, notificacao, notification, +1 moeda, coin, missao concluida, quest complete, show hud message, numeros de dano, damage numbers, numero flutuante, floating number, critico, critical, cura verde, heal, show damage number, mira, crosshair, reticula, spread, abertura da mira, set crosshair spread, direcao do dano, damage direction, indicador de dano, de onde veio o tiro, minimapa, minimap, mapa, radar, set hud visible, esconder hud, hide hud, modo de foto, photo mode, set photo mode, area segura, safe area, notch, escala do hud, hud scale, resolucao, resolution
--- pt
O HUD do jogo montado com **peças prontas** (widgets nativos do UMG, configurados no Designer e ligados por Grafo ou código):
1. No personagem do jogador: **Details > Adicionar > HUD Layout** (no Grafo: **Componentes > + Adicionar > Jogabilidade**). No Play ele mostra o **WBP_GameHUD** (Content/UI, criado quando falta) com todas as peças no lugar; troque pelo seu em **Widget do HUD**. Ele escala o HUD pela resolução (**Escalar Por**: altura da tela, lado menor, caber em 1920 x 1080, pixels fixos; **Escala do HUD**), deixa as **Margens Seguras** livres e, no Android, a **Área Segura do Aparelho**; some no **Modo de Foto** e quando uma **Sequência** controla a câmera; em **Peças Mostradas** liga e desliga cada tipo (o minimapa vem desligado).
2. As peças (Paleta > **HUD** no Designer de qualquer Widget Graph):
   - **Barra de Atributo**: o **Atributo** de um ator (vazio: o jogador): `Health`, `Shield` ou qualquer variável float do Grafo (o máximo: `Max<Nome>`, ex. `Stamina` e `MaxStamina`). A perda é suavizada, o **trecho perdido** fica claro um momento e escoa, a cura aparece na **Cor do Ganho**; números, segmentos e o pulso de vida baixa.
   - **Bússola**: os pontos cardeais girando com a vista (o norte é +X) e os marcadores com a distância.
   - **Mensagens**: fila com **Máximo Mostrado** e **Duração**; o mesmo texto se empilha (`+1 Moeda` duas vezes vira `+2 Moeda`).
   - **Números de Dano**: os da Vida e os do **Show Damage Number** (crítico maior com `!`, cura em verde `+25`), nas cores do widget.
   - **Mira**: quatro linhas e um ponto que abrem com o **Set Crosshair Spread** e com a velocidade do jogador.
   - **Marcadores na Tela**: losango, rótulo e distância sobre o ator; fora da vista ficam na **borda com uma seta**.
   - **Direção do Dano**: arcos em volta do centro apontando para quem acertou o jogador.
   - **Minimapa**: a vista de cima em volta do jogador (marcadores e atores com Vida), girando com a vista ou com o norte para cima.
3. Nós (categoria **User Interface > HUD**): **Show HUD Message**, **Add Compass Marker**, **Remove Compass Marker**, **Add Screen Marker**, **Remove Screen Marker**, **Show Damage Number**, **Set HUD Visible**, **Bind Attribute Bar** (a barra de um chefe: `Bar Name` + `Actor` + `Attribute`), **Set Crosshair Spread**, **Set Photo Mode**, **Is HUD Visible**. Nos scripts entram com o mesmo nome (`ShowHUDMessage`, `AddCompassMarker`...).

```diagram steps
HUD Layout no jogador | Play: WBP_GameHUD na tela (escala, área segura) | Add Compass / Screen Marker no objetivo | Show HUD Message "+1 Moeda" | Apply Damage: a barra perde, o trecho perdido escoa, a seta mostra de onde veio
```
```blueprint
Event BeginPlay -> Add Compass Marker ( Label = "Saída" ) -> Add Screen Marker ( Label = "Saída", Height Offset = 150 )
Event ActorBeginOverlap -> Show HUD Message ( Text = "+1 Moeda", Duration = 2 ) -> Show Damage Number ( Location = Get Actor Location, Amount = 25, Critical = true )
Event On Interact ( Instigator ) -> Show HUD Message ( Text = "Missão concluída" ) -> Remove Compass Marker -> Remove Screen Marker
```
```real
class Objective extends Actor
    event BeginPlay()
        AddCompassMarker("Saída", MakeLinearColor(1.0, 0.8, 0.25, 1.0), true)
        AddScreenMarker("Saída", MakeLinearColor(1.0, 0.8, 0.25, 1.0), 150.0, true, true)

    event OnBeginOverlap(other: Actor)
        ShowHUDMessage("+1 Moeda", 2.0, MakeLinearColor(1.0, 1.0, 1.0, 1.0))
        ShowDamageNumber(GetActorLocation(), 25.0, true, false)

    event OnInteract(instigator: Actor)
        ShowHUDMessage("Missão concluída", 3.0, MakeLinearColor(1.0, 0.85, 0.3, 1.0))
        RemoveCompassMarker()
        RemoveScreenMarker()
```

O custo: o HUD inteiro com 100 marcadores e 50 números de dano custa ~0,17 ms de CPU e ~0,01 ms de GPU por quadro; a lista **HUD** do Debug Viewer (F10) mostra o tempo de cada peça e de cada marcador. Para a vida, veja [Saúde e Dano](article:health-damage); para montar widgets, [Interface do jogo](article:game-ui).
--- en
The game's HUD built from **ready-made pieces** (native UMG widgets, set up in the Designer and driven by Graphs or code):
1. On the player's pawn: **Details > Add > HUD Layout** (in a Graph: **Components > + Add > Gameplay**). In Play it shows **WBP_GameHUD** (Content/UI, made when missing) with every piece in place; swap in your own with **HUD Widget**. It scales the HUD by the resolution (**Scale By**: screen height, shortest side, fit 1920 x 1080, fixed pixels; **HUD Scale**), keeps the **Safe Margins** free and, on Android, the **Device Safe Area**; it hides in the **Photo Mode** and while a **Sequence** has the camera; **Pieces Shown** turns each kind on and off (the minimap starts off).
2. The pieces (Palette > **HUD** in any Widget Graph's Designer):
   - **Attribute Bar**: an actor's **Attribute** (empty: the player): `Health`, `Shield` or any float variable of its Graph (its max: `Max<Name>`, e.g. `Stamina` and `MaxStamina`). The loss is smoothed, the **lost chunk** stays light a moment and drains, a heal shows in the **Gain Color**; numbers, segments and the low-health pulse.
   - **Compass**: the cardinal points turning with the view (north is +X) and the markers with their distance.
   - **Message Feed**: a queue with **Max Shown** and **Duration**; the same text stacks (`+1 Coin` twice becomes `+2 Coin`).
   - **Damage Numbers**: the Health's and **Show Damage Number**'s (critical bigger with `!`, heal in green `+25`), in the widget's colors.
   - **Crosshair**: four lines and a dot opening with **Set Crosshair Spread** and the player's speed.
   - **Screen Markers**: a diamond, a label and the distance over the actor; out of the view they wait at the **edge with an arrow**.
   - **Damage Direction**: arcs around the middle pointing at who hit the player.
   - **Minimap**: the view from above around the player (the markers, the actors with Health), turning with the view or north up.
3. Nodes (category **User Interface > HUD**): **Show HUD Message**, **Add Compass Marker**, **Remove Compass Marker**, **Add Screen Marker**, **Remove Screen Marker**, **Show Damage Number**, **Set HUD Visible**, **Bind Attribute Bar** (a boss's bar: `Bar Name` + `Actor` + `Attribute`), **Set Crosshair Spread**, **Set Photo Mode**, **Is HUD Visible**. In scripts they have the same names (`ShowHUDMessage`, `AddCompassMarker`...).

```diagram steps
HUD Layout on the player | Play: WBP_GameHUD on the screen (scale, safe area) | Add Compass / Screen Marker on the objective | Show HUD Message "+1 Coin" | Apply Damage: the bar drops, the lost chunk drains, the arc shows where it came from
```
```blueprint
Event BeginPlay -> Add Compass Marker ( Label = "Exit" ) -> Add Screen Marker ( Label = "Exit", Height Offset = 150 )
Event ActorBeginOverlap -> Show HUD Message ( Text = "+1 Coin", Duration = 2 ) -> Show Damage Number ( Location = Get Actor Location, Amount = 25, Critical = true )
Event On Interact ( Instigator ) -> Show HUD Message ( Text = "Quest Complete" ) -> Remove Compass Marker -> Remove Screen Marker
```
```real
class Objective extends Actor
    event BeginPlay()
        AddCompassMarker("Exit", MakeLinearColor(1.0, 0.8, 0.25, 1.0), true)
        AddScreenMarker("Exit", MakeLinearColor(1.0, 0.8, 0.25, 1.0), 150.0, true, true)

    event OnBeginOverlap(other: Actor)
        ShowHUDMessage("+1 Coin", 2.0, MakeLinearColor(1.0, 1.0, 1.0, 1.0))
        ShowDamageNumber(GetActorLocation(), 25.0, true, false)

    event OnInteract(instigator: Actor)
        ShowHUDMessage("Quest Complete", 3.0, MakeLinearColor(1.0, 0.85, 0.3, 1.0))
        RemoveCompassMarker()
        RemoveScreenMarker()
```

The cost: the whole HUD with 100 markers and 50 damage numbers takes ~0.17 ms of CPU and ~0.01 ms of GPU a frame; the Debug Viewer's (F10) **HUD** list shows each piece's and each marker's time. For the health, see [Health and Damage](article:health-damage); to build widgets, [Game UI](article:game-ui).
--- es
El HUD del juego construido con **piezas listas** (widgets nativos de UMG, configurados en el Designer y controlados por Grafos o código):
1. En el peón del jugador: **Detalles > Añadir > HUD Layout** (en un Grafo: **Components > + Add > Gameplay**). Al Jugar muestra **WBP_GameHUD** (Content/UI, creado si falta) con todas las piezas en su lugar; cámbialo por el tuyo con **HUD Widget**. Escala el HUD según la resolución (**Scale By**: altura de pantalla, lado más corto, ajustar a 1920 x 1080, píxeles fijos; **HUD Scale**), mantiene libres los **Safe Margins** y, en Android, el **Device Safe Area**; se oculta en el **Photo Mode** y mientras una **Sequence** tiene la cámara; **Pieces Shown** activa y desactiva cada tipo (el minimapa empieza apagado).
2. Las piezas (Paleta > **HUD** en el Designer de cualquier Widget Graph):
   - **Attribute Bar**: el **Attribute** de un actor (vacío: el jugador): `Health`, `Shield` o cualquier variable float de su Grafo (su máximo: `Max<Nombre>`, p. ej. `Stamina` y `MaxStamina`). La pérdida se suaviza, el **trozo perdido** queda claro un momento y se drena, una curación se muestra en el **Gain Color**; números, segmentos y el pulso de poca vida.
   - **Compass**: los puntos cardinales girando con la vista (el norte es +X) y los marcadores con su distancia.
   - **Message Feed**: una cola con **Max Shown** y **Duration**; el mismo texto se apila (`+1 Coin` dos veces pasa a `+2 Coin`).
   - **Damage Numbers**: los de la Vida y los de **Show Damage Number** (crítico más grande con `!`, curación en verde `+25`), en los colores del widget.
   - **Crosshair**: cuatro líneas y un punto que se abren con **Set Crosshair Spread** y la velocidad del jugador.
   - **Screen Markers**: un rombo, una etiqueta y la distancia sobre el actor; fuera de la vista esperan en el **borde con una flecha**.
   - **Damage Direction**: arcos alrededor del centro que apuntan a quien golpeó al jugador.
   - **Minimap**: la vista desde arriba alrededor del jugador (los marcadores, los actores con Health), girando con la vista o con el norte arriba.
3. Nodos (categoría **User Interface > HUD**): **Show HUD Message**, **Add Compass Marker**, **Remove Compass Marker**, **Add Screen Marker**, **Remove Screen Marker**, **Show Damage Number**, **Set HUD Visible**, **Bind Attribute Bar** (la barra de un jefe: `Bar Name` + `Actor` + `Attribute`), **Set Crosshair Spread**, **Set Photo Mode**, **Is HUD Visible**. En los scripts tienen los mismos nombres (`ShowHUDMessage`, `AddCompassMarker`...).

```diagram steps
HUD Layout en el jugador | Jugar: WBP_GameHUD en pantalla (escala, área segura) | Add Compass / Screen Marker en el objetivo | Show HUD Message "+1 Coin" | Apply Damage: la barra baja, el trozo perdido se drena, el arco muestra de dónde vino
```
```blueprint
Event BeginPlay -> Add Compass Marker ( Label = "Exit" ) -> Add Screen Marker ( Label = "Exit", Height Offset = 150 )
Event ActorBeginOverlap -> Show HUD Message ( Text = "+1 Coin", Duration = 2 ) -> Show Damage Number ( Location = Get Actor Location, Amount = 25, Critical = true )
Event On Interact ( Instigator ) -> Show HUD Message ( Text = "Quest Complete" ) -> Remove Compass Marker -> Remove Screen Marker
```
```real
class Objective extends Actor
    event BeginPlay()
        AddCompassMarker("Exit", MakeLinearColor(1.0, 0.8, 0.25, 1.0), true)
        AddScreenMarker("Exit", MakeLinearColor(1.0, 0.8, 0.25, 1.0), 150.0, true, true)

    event OnBeginOverlap(other: Actor)
        ShowHUDMessage("+1 Coin", 2.0, MakeLinearColor(1.0, 1.0, 1.0, 1.0))
        ShowDamageNumber(GetActorLocation(), 25.0, true, false)

    event OnInteract(instigator: Actor)
        ShowHUDMessage("Quest Complete", 3.0, MakeLinearColor(1.0, 0.85, 0.3, 1.0))
        RemoveCompassMarker()
        RemoveScreenMarker()
```

El costo: todo el HUD con 100 marcadores y 50 números de daño toma ~0.17 ms de CPU y ~0.01 ms de GPU por cuadro; la lista **HUD** del Debug Viewer (F10) muestra el tiempo de cada pieza y de cada marcador. Para la vida, consulta [Salud y daño](article:health-damage); para construir widgets, [UI del juego](article:game-ui).
