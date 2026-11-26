id: debug-viewer
kind: howto
title: Debug Viewer: o que está pesando | Debug Viewer: what is heavy | Debug Viewer: qué está pesando
keys: debug viewer, f10, pesado, lento, travando, custo, ms, perfil, profiler, o que pesa, lag, gargalo, qual objeto pesa
--- pt
```diagram steps
F10 abre (editor e jogo) | Escolha uma lista | Os mais caros em vermelho | Clique: seleciona o ator
```
- **F10** (ou **Janela > Debug Viewer**) abre a janela no editor e no jogo empacotado. [Abrir](open:debug) [Mostrar onde](show:Window>Debug Viewer)
- Cada lista é um sistema com o custo na frente: **Malhas**, **Iluminação**, **Shaders**, **Animação**, **Grafos**, **Física**, **Sistemas de Jogo**.
- Dentro, os objetos da cena com o custo de cada um. Vermelho = pesado (acima de 0,5 ms ou de 40% da lista); amarelo = atenção.
- No editor, clicar num item seleciona o ator: dá para apagar, trocar o LOD ou simplificar o material.
- `≈` quer dizer que o total medido foi repartido por um modelo de custo (triângulos, área na tela, luzes).
- Fechado, não custa nada; aberto, ele mostra o próprio custo no rodapé.
- Veja também: [Desempenho](article:performance), [Orçamento do quadro](article:performance-budget).
--- en
```diagram steps
F10 opens it (editor and game) | Pick a list | The costly ones in red | Click: selects the actor
```
- **F10** (or **Window > Debug Viewer**) opens the window in the editor and in the packaged game. [Open](open:debug) [Show me where](show:Window>Debug Viewer)
- Each list is a system with its cost in front: **Meshes**, **Lighting**, **Shaders**, **Animation**, **Graphs**, **Physics**, **Game Systems**.
- Inside, the scene's objects with each one's cost. Red = heavy (over 0.5 ms or 40% of the list); yellow = watch it.
- In the editor, clicking an item selects the actor: delete it, change its LOD or simplify its material.
- `≈` means the measured total was split by a cost model (triangles, screen area, lights).
- Closed it costs nothing; open, it shows its own cost at the bottom.
- See also: [Performance](article:performance), [Frame budget](article:performance-budget).
--- es
```diagram steps
F10 lo abre (editor y juego) | Elige una lista | Los costosos en rojo | Clic: selecciona el actor
```
- **F10** (o **Ventana > Debug Viewer**) abre la ventana en el editor y en el juego empaquetado. [Abrir](open:debug) [Mostrar dónde](show:Window>Debug Viewer)
- Cada lista es un sistema con su costo al frente: **Meshes**, **Lighting**, **Shaders**, **Animation**, **Graphs**, **Physics**, **Game Systems**.
- Dentro, los objetos de la escena con el costo de cada uno. Rojo = pesado (más de 0,5 ms o 40% de la lista); amarillo = ojo con él.
- En el editor, hacer clic en un ítem selecciona el actor: bórralo, cambia su LOD o simplifica su material.
- `≈` significa que el total medido se repartió con un modelo de costo (triángulos, área en pantalla, luces).
- Cerrado no cuesta nada; abierto, muestra su propio costo al pie.
- Mira también: [Rendimiento](article:performance), [Presupuesto de fotograma](article:performance-budget).
