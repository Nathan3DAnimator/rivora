id: actor-icons
kind: howto
title: Ícones dos atores (o modelo de cada um) | Actor icons (each one's model) | Íconos de los actores
keys: icone, icon, icones, icons, esconder icone, hide icon, mostrar icone, show icon, icone do ator, actor icon, billboard, sprite do editor, miniatura, thumbnail, icone dinamico, dynamic icon, poluicao visual, clutter, organizador, outliner, t pose
--- pt
```diagram steps
Organizador e viewport: o ícone é o próprio modelo | Detalhes > Transform > Mostrar Ícone (cada ator) | Ator > Visibilidade > Esconder/Mostrar Ícones dos Selecionados | G: modo jogo, sem nenhum ícone
```
## Como funciona
Os ícones mudam com o ator:
- **Malha importada**: o ícone é o **próprio modelo** (no Organizador e no viewport).
- **Skeletal Mesh**: o modelo na pose de repouso (a **T pose** dele).
- Luzes, câmeras, Player Start, sons e os outros: o ícone do tipo (as luzes na cor delas). As formas básicas (cubo, esfera...) não têm ícone no viewport.

Cada ator escolhe se mostra o ícone: **Detalhes > Transform > Mostrar Ícone**. Para vários de uma vez, selecione e use **Ator > Visibilidade > Esconder Ícones dos Selecionados**. Desligar o ícone de um ator esconde também os das partes dele. **G** esconde todos (modo jogo).
--- en
```diagram steps
Outliner and viewport: the icon is the model itself | Details > Transform > Show Icon (each actor) | Actor > Visibility > Hide/Show Icons of Selected | G: game view, no icons at all
```
## How it works
The icons follow the actor:
- **Imported mesh**: the icon is the **model itself** (in the Outliner and in the viewport).
- **Skeletal Mesh**: the model in its rest pose (its **T pose**).
- Lights, cameras, Player Start, sounds and the rest: the kind's icon (lights in their color). The basic shapes (cube, sphere...) have no viewport icon.

Each actor chooses whether its icon shows: **Details > Transform > Show Icon**. For many at once, select them and use **Actor > Visibility > Hide Icons of Selected**. Turning an actor's icon off hides its parts' icons too. **G** hides them all (game view).
--- es
```diagram steps
Outliner y ventana gráfica: el ícono es el propio modelo | Detalles > Transform > Show Icon (cada actor) | Actor > Visibility > Ocultar/Mostrar íconos de la selección | G: vista de juego, sin ningún ícono
```
## Cómo funciona
Los íconos siguen al actor:
- **Malla importada**: el ícono es el **propio modelo** (en el Outliner y en la ventana gráfica).
- **Skeletal Mesh**: el modelo en su pose de reposo (su **pose en T**).
- Luces, cámaras, Player Start, sonidos y el resto: el ícono del tipo (las luces en su color). Las formas básicas (cubo, esfera...) no tienen ícono en la ventana gráfica.

Cada actor elige si muestra su ícono: **Detalles > Transform > Show Icon**. Para varios a la vez, selecciónalos y usa **Actor > Visibility > Ocultar íconos de la selección**. Apagar el ícono de un actor oculta también los íconos de sus partes. **G** los oculta todos (vista de juego).
