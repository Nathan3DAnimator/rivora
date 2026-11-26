id: selection-outline
kind: howto
title: Contorno da seleção | Selection outline
keys: contorno, contorno da seleção, selection outline, outline, destaque, highlight, selecionado, selected, borda laranja, orange border, modo jogo, game view, g, ícones escondidos, icons hidden, skeletal mesh selecionada, blueprint selecionado, empty, player start, trigger
--- pt
Ao selecionar um ator, a viewport desenha uma **borda laranja** em volta dele, como na Unreal:
- **Qualquer ator com malha**: Static Mesh, **Skeletal Mesh** (a pose animada de agora, não a de ligação), **Blueprint com componentes**, veículos e Landscape. Todas as malhas que pendem do ator ganham **uma borda só**, em volta do ator inteiro.
- **Materiais Masked** (folhas, grades): a borda segue o recorte do alfa, não o cartão.
- **Cabelo (Groom)** e **multidão** (Crowd Spawner: todos os agentes, desenhados juntos) entram no mesmo contorno do ator. **Água** e **partículas** mostram a caixa delas em laranja.
- **Atores sem malha** (luz, câmera, volumes, **Empty**, **Player Start**, trigger): a forma deles em laranja (o alcance da luz, a caixa ou a cápsula da colisão; uma caixa pequena no Empty), para **todos** os selecionados, não só o último.
- Vale também com os ícones escondidos (**G**, modo jogo) e na saída **HDR10**.
- Sem nada selecionado, o passe do contorno nem roda (custo zero); com seleção, fica abaixo de 0,3 ms em 1440p.

```diagram steps
Selecionar ator | as malhas dele e das partes | máscara com o id do ator | borda laranja
```
- Veja também: [Selecionar atores](article:select-actors), [Ícones dos atores](article:actor-icons).
--- en
Selecting an actor draws an **orange border** around it in the viewport, like Unreal:
- **Any actor with a mesh**: Static Mesh, **Skeletal Mesh** (its animated pose now, not its bind), **Blueprint with components**, vehicles and Landscape. Every mesh hanging from the actor gets **one border**, around the whole actor.
- **Masked materials** (leaves, fences): the border follows the alpha's cut, not the card.
- **Hair (Groom)** and **crowds** (Crowd Spawner: every agent, drawn together) join the actor's outline. **Water** and **particles** show their box in orange.
- **Actors with no mesh** (light, camera, volumes, **Empty**, **Player Start**, trigger): their shape in orange (the light's reach, the collision's box or capsule; a small box on an Empty), for **every** selected one, not just the last.
- It holds with the icons hidden (**G**, game view) and on **HDR10** output too.
- With nothing selected the outline's pass doesn't run (no cost); with a selection it stays under 0.3 ms at 1440p.

```diagram steps
Select actor | its meshes and its parts' | mask with the actor's id | orange border
```
- See also: [Select actors](article:select-actors), [Actor icons](article:actor-icons).
--- es
Al seleccionar un actor, el viewport dibuja un **borde naranja** a su alrededor, como en Unreal:
- **Cualquier actor con malla**: Static Mesh, **Skeletal Mesh** (su pose animada de ahora, no la de enlace), **Blueprint con componentes**, vehículos y Landscape. Todas las mallas que cuelgan del actor tienen **un solo borde**, alrededor del actor entero.
- **Materiales Masked** (hojas, rejas): el borde sigue el recorte del alfa, no la tarjeta.
- **Cabello (Groom)** y **multitudes** (Crowd Spawner: todos los agentes, dibujados juntos) entran en el contorno del actor. El **agua** y las **partículas** muestran su caja en naranja.
- **Actores sin malla** (luz, cámara, volúmenes, **Empty**, **Player Start**, trigger): su forma en naranja (el alcance de la luz, la caja o la cápsula de la colisión; una caja pequeña en el Empty), para **todos** los seleccionados, no solo el último.
- También con los iconos ocultos (**G**, modo juego) y en la salida **HDR10**.
- Sin nada seleccionado, la pasada del contorno no se ejecuta (costo cero); con selección queda por debajo de 0,3 ms en 1440p.

```diagram steps
Seleccionar actor | sus mallas y las de sus partes | máscara con el id del actor | borde naranja
```
- Véase también: [Seleccionar actores](article:select-actors), [Iconos de los actores](article:actor-icons).