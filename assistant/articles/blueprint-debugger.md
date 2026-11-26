id: blueprint-debugger
kind: howto
title: Depurador de Grafo: breakpoints, passos, valores | Graph Debugger: breakpoints, steps, values | Depurador de Grafo: breakpoints, pasos, valores
keys: depurador, debugger, blueprint debugger, breakpoint, breakpoints, ponto de parada, parar no no, pausar no no, f9, step over, step into, step out, passo a passo, executar passo a passo, f10, f11, f5, continuar, resume, watch, observar valor, watch this value, valor do pino, pin value, valor da variavel, call stack, pilha de chamadas, debug object, objeto de depuracao, qual instancia, fios piscando, execution flow, depurar script, breakpoint no script, breakpoint na linha, grafo, grafos, graph, graphs
--- pt
Como o depurador de Grafo da Unreal: o jogo **para num nó**, o mundo espera e você vê os valores e anda nó a nó.
1. No editor do Grafo, selecione um nó e aperte **F9** (ou clique direito > **Adicionar Breakpoint**). Aparece um **ponto vermelho** no canto do nó; vazado = desligado.
2. **Jogue** (Alt+P). Quando a execução chega no nó, o Play **para antes de rodá-lo**: o mundo não anda, o editor do Grafo vem para a frente com o nó **contornado de amarelo** e o mouse fica livre.
3. Veja os valores: **passe o mouse num pino** (aparece `= valor`); clique direito no pino > **Observar Este Valor** põe ele na janela **Depurador de Grafo** (botão **Depurar** na barra), que mostra também as **variáveis** da instância e a **pilha de chamadas** (funções, Custom Events, eventos de outros atores; clique leva ao nó).
4. Ande: **F10 Passar por Cima** (roda o nó e para no próximo do mesmo grafo), **F11 Entrar** (entra na função ou no evento do outro ator), **Shift+F11 Sair** (até o fim da função), **F5 Continuar** (até o próximo breakpoint).
5. Vários atores da mesma classe? Na barra, **Objeto de Depuração** escolhe qual instância para e mostra os valores; selecionar um ator dela no nível também escolhe. Enquanto joga, os **fios de execução piscam** por onde a execução passou.
6. **Scripts** (Rivora Script, C#, Python, C): clique na **margem** do Editor de Código (ou F9 na linha). O jogo para no primeiro nó daquela linha, a linha fica amarela, e o passo anda **linha a linha**.

```diagram keys
F9: breakpoint no nó | F5: continuar | F10: passar por cima | F11: entrar | Shift+F11: sair
```
```diagram steps
F9 no nó | Alt+P | para no nó (mundo parado) | mouse no pino: valor | F10 / F11 / F5
```
Custo: nenhum sem breakpoint (o nó marcado troca a operação dele por uma parada; o resto do código não muda). Um breakpoint num evento que roda fora do jogo (o clique de um widget) só avisa no log, não para.
--- en
Like Unreal's Graph debugger: the game **stops at a node**, the world waits and you see the values and go node by node.
1. In the Graph editor, select a node and press **F9** (or right click > **Add Breakpoint**). A **red dot** shows on the node's corner; hollow = disabled.
2. **Play** (Alt+P). When the execution reaches the node, the Play **stops before running it**: the world doesn't move, the Graph editor comes forward with the node **outlined in yellow** and the mouse is free.
3. See the values: **hover a pin** (it shows `= value`); right click the pin > **Watch This Value** puts it in the **Graph Debugger** window (the **Debug** button on the toolbar), which also shows the instance's **variables** and the **call stack** (functions, Custom Events, other actors' events; a click goes to the node).
4. Step: **F10 Step Over** (runs the node, stops at the next one in the same graph), **F11 Step Into** (into the function or the other actor's event), **Shift+F11 Step Out** (to the end of the function), **F5 Resume** (to the next breakpoint).
5. Several actors of the class? On the toolbar, **Debug Object** picks which instance stops and shows its values; selecting one of its actors in the level picks it too. While playing, the **execution wires flash** where the execution went.
6. **Scripts** (Rivora Script, C#, Python, C): click the Code Editor's **gutter** (or F9 on the line). The game stops at that line's first node, the line turns yellow, and stepping goes **line by line**.

```diagram keys
F9: breakpoint on the node | F5: resume | F10: step over | F11: step into | Shift+F11: step out
```
```diagram steps
F9 on the node | Alt+P | stops at the node (world paused) | mouse on a pin: value | F10 / F11 / F5
```
Cost: none without a breakpoint (the marked node's operation is swapped for a stop; the rest of the code doesn't change). A breakpoint in an event that runs outside the game (a widget's click) only warns in the log, it doesn't stop.
--- es
Como el depurador de Grafo de Unreal: el juego **se detiene en un nodo**, el mundo espera y ves los valores y avanzas nodo por nodo.
1. En el editor de Grafo, selecciona un nodo y pulsa **F9** (o clic derecho > **Add Breakpoint**). Aparece un **punto rojo** en la esquina del nodo; hueco = desactivado.
2. **Jugar** (Alt+P). Cuando la ejecución llega al nodo, el Play **se detiene antes de ejecutarlo**: el mundo no se mueve, el editor de Grafo pasa al frente con el nodo **resaltado en amarillo** y el mouse queda libre.
3. Mira los valores: **pasa el mouse sobre un pin** (muestra `= valor`); clic derecho en el pin > **Watch This Value** lo pone en la ventana **Graph Debugger** (el botón **Debug** de la barra de herramientas), que también muestra las **variables** de la instancia y la **pila de llamadas** (funciones, Custom Events, eventos de otros actores; un clic lleva al nodo).
4. Avanza: **F10 Step Over** (ejecuta el nodo, se detiene en el siguiente del mismo grafo), **F11 Step Into** (entra en la función o en el evento del otro actor), **Shift+F11 Step Out** (hasta el final de la función), **F5 Resume** (hasta el siguiente breakpoint).
5. ¿Varios actores de la clase? En la barra de herramientas, **Debug Object** elige qué instancia se detiene y muestra sus valores; seleccionar uno de sus actores en el nivel también la elige. Mientras juegas, los **cables de ejecución parpadean** por donde pasó la ejecución.
6. **Scripts** (Rivora Script, C#, Python, C): haz clic en el **margen** del Editor de Código (o F9 en la línea). El juego se detiene en el primer nodo de esa línea, la línea se pone amarilla y los pasos van **línea por línea**.

```diagram keys
F9: breakpoint en el nodo | F5: continuar | F10: step over | F11: step into | Shift+F11: step out
```
```diagram steps
F9 en el nodo | Alt+P | se detiene en el nodo (mundo en pausa) | mouse sobre un pin: valor | F10 / F11 / F5
```
Costo: ninguno sin breakpoint (la operación del nodo marcado se cambia por una parada; el resto del código no cambia). Un breakpoint en un evento que se ejecuta fuera del juego (el clic de un widget) solo avisa en el registro, no se detiene.
