id: background-tasks
kind: concept
title: Tarefas em segundo plano (os cartões) | Background tasks (the cards) | Tareas en segundo plano (las tarjetas)
keys: travando, carregando, barra de progresso, cartao, tarefa, cancelar importacao, demora, async, loading, editor travou, congelou
--- pt
```diagram steps
Você importa, assa ou empacota | Um worker faz o trabalho | O cartão mostra o progresso | Pronto: o resultado entra no editor
```
O editor não para enquanto trabalha: importar, recarregar malhas, compilar materiais, assar a luz e empacotar rodam em segundo plano.
- Cada tarefa aparece como um **cartão no canto inferior direito**: o que faz, a barra, o tempo e o que falta.
- **Arraste o cabeçalho** para mover os cartões; o botão do cabeçalho deixa todos **finos** (só a barra).
- O **X** cancela uma tarefa enquanto roda (e fecha o cartão depois).
--- en
```diagram steps
You import, bake or package | A worker does the work | The card shows the progress | Done: the result goes into the editor
```
The editor doesn't stop while it works: importing, reloading meshes, compiling materials, baking light and packaging run in the background.
- Each task shows as a **card at the bottom right**: what it does, its bar, the time and what's left.
- **Drag the header** to move the cards; the header's button makes them all **thin** (only the bar).
- The **X** cancels a task while it runs (and closes the card after).
--- es
```diagram steps
Importas, horneas o empaquetas | Un worker hace el trabajo | La tarjeta muestra el progreso | Listo: el resultado entra al editor
```
El editor no se detiene mientras trabaja: importar, recargar mallas, compilar materiales, hornear luz y empaquetar se ejecutan en segundo plano.
- Cada tarea aparece como una **tarjeta abajo a la derecha**: qué hace, su barra, el tiempo y lo que falta.
- **Arrastra el encabezado** para mover las tarjetas; el botón del encabezado las vuelve todas **finas** (solo la barra).
- La **X** cancela una tarea mientras se ejecuta (y cierra la tarjeta después).
