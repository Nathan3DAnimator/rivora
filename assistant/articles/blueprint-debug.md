id: blueprint-debug
kind: howto
title: Depurar Grafos | Debug Graphs | Depurar Grafos
keys: depurar blueprint, debug blueprint, nao funciona, nao roda, blueprint nao faz nada, compile error, erro de compilacao, print string, grafo, grafos, graph, graphs
--- pt
1. **Compile** e veja os erros no painel de baixo (o nó com problema fica marcado).
2. Coloque **Print String** nos caminhos para ver o que roda.
3. O evento roda? Confira se o ator está no nível, se é da classe certa e, para overlaps, se tem colisão.
4. Laço que trava? Uma execução passa de 1 milhão de operações e é parada com uma mensagem. [Laço infinito](article:error-infinite-loop)
5. Pare no nó e veja os valores: **F9** põe um breakpoint, F10 / F11 andam um passo. [Depurador de Grafo](article:blueprint-debugger)
--- en
1. **Compile** and read the errors in the bottom panel (the faulty node is marked).
2. Put **Print String** on the paths to see what runs.
3. Does the event run? Check the actor is in the level, of the right class and, for overlaps, that it has collision.
4. A loop that hangs? A run past 1 million operations is stopped with a message. [Infinite loop](article:error-infinite-loop)
5. Stop at the node and see the values: **F9** sets a breakpoint, F10 / F11 take a step. [Graph Debugger](article:blueprint-debugger)
--- es
1. **Compila** y lee los errores en el panel inferior (el nodo con falla queda marcado).
2. Pon **Print String** en los caminos para ver qué se ejecuta.
3. ¿El evento se ejecuta? Comprueba que el actor esté en el nivel, que sea de la clase correcta y, en los solapamientos, que tenga colisión.
4. ¿Un bucle que se cuelga? Una ejecución de más de 1 millón de operaciones se detiene con un mensaje. [Bucle infinito](article:error-infinite-loop)
5. Detente en el nodo y mira los valores: **F9** pone un punto de interrupción, F10 / F11 dan un paso. [Depurador de Grafos](article:blueprint-debugger)
