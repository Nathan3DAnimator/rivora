id: blueprint-functions
kind: howto
title: Funções, macros e Custom Events nos Grafos | Functions, macros and Custom Events in Graphs | Funciones, macros y Custom Events en los Grafos
keys: funcao blueprint, function, custom event, evento personalizado, chamar evento, return node, parametros, entrada saida, macro, reutilizar logica, grafo, grafos, graph, graphs
--- pt
- **Função**: entradas e saídas (Return Node), roda na hora. Crie no painel My Grafo > Functions.
- **Custom Event**: sem retorno; pode ser chamado pelo nome por widgets, Sequencer, timers, notifies e outros Grafos. Aceita **Delay** dentro.
- Funções puras (sem pino branco) são para cálculos; as outras têm efeito.
- Em código: um método sem parâmetros e sem retorno vira Custom Event; com parâmetros ou retorno vira função.
--- en
- **Function**: inputs and outputs (Return Node), runs at once. Make one in My Graph > Functions.
- **Custom Event**: no return; can be called by name from widgets, the Sequencer, timers, notifies and other Graphs. It allows a **Delay** inside.
- Pure functions (no white pin) are for computing; the others have effects.
- In code: a method without parameters and return becomes a Custom Event; with parameters or a return, a function.
--- es
- **Función**: entradas y salidas (Return Node), se ejecuta al instante. Crea una en My Graph > Functions.
- **Custom Event**: sin retorno; se puede llamar por nombre desde widgets, el Sequencer, temporizadores, notifies y otros Grafos. Permite un **Delay** dentro.
- Las funciones puras (sin pin blanco) sirven para calcular; las demás tienen efectos.
- En código: un método sin parámetros ni retorno se convierte en Custom Event; con parámetros o retorno, en función.
