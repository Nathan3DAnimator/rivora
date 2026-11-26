id: game-ui
kind: howto
title: Interface do jogo (HUD, menus) | The game's UI (HUD, menus) | La UI del juego (HUD, menús)
keys: interface, hud, menu, botao, barra de vida, texto na tela, widget, umg, tela de pausa, ui
--- pt
```diagram steps
Adicionar > Widget Blueprint | Designer: montar a tela | Ligar valores e cliques | Create Widget + Add to Viewport
```
1. Navegador: **Adicionar > Widget Graph**. No **Designer**, arraste Canvas, Text, Image, Button, Progress Bar...
2. Ligue os valores (uma barra de vida a uma variável) e os cliques (Custom Events).
3. Mostre na tela: **Create Widget** e **Add to Viewport** (no BeginPlay do personagem, por exemplo).
--- en
```diagram steps
Add > Widget Blueprint | Designer: lay out the screen | Bind values and clicks | Create Widget + Add to Viewport
```
1. Browser: **Add > Widget Graph**. In the **Designer**, drag Canvas, Text, Image, Button, Progress Bar...
2. Bind the values (a health bar to a variable) and the clicks (Custom Events).
3. Put it on the screen: **Create Widget** and **Add to Viewport** (in the character's BeginPlay, for example).
--- es
```diagram steps
Añadir > Widget Blueprint | Designer: diseña la pantalla | Enlaza valores y clics | Create Widget + Add to Viewport
```
1. Explorador: **Añadir > Widget Graph**. En el **Designer**, arrastra Canvas, Text, Image, Button, Progress Bar...
2. Enlaza los valores (una barra de salud a una variable) y los clics (Custom Events).
3. Ponlo en pantalla: **Create Widget** y **Add to Viewport** (en el BeginPlay del personaje, por ejemplo).
