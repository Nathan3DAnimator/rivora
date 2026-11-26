id: jit-native
kind: concept
title: Código nativo (JIT) de Grafos e scripts | Native code (JIT) of Graphs and scripts | Código nativo (JIT) de Grafos y scripts
keys: jit, nativo, native, compilacao nativa, performance de script, blueprint lento, vm, bytecode, interpretador, x64, sse, realengine_jit, grafo, grafos, graph, graphs
--- pt
```diagram bars ms
Interpretado: 3.16
Nativo (JIT): 0.40
```
- Grafos e scripts compilam para um **bytecode de registradores**; na primeira execução, um **JIT x64** transforma as operações numéricas (movimentos, saltos, aritmética, comparações, vetores) em código de máquina SSE.
- O resto (chamadas, eventos, strings, arrays) volta ao interpretador uma operação por vez: o comportamento é idêntico.
- Num laço aritmético de 40 mil voltas: 3,2 ms interpretado, 0,4 ms nativo (8×).
- `REALENGINE_JIT=0` desliga (para comparar ou isolar um problema).
--- en
```diagram bars ms
Interpreted: 3.16
Native (JIT): 0.40
```
- Graphs and scripts compile to a **register bytecode**; on the first run, an **x64 JIT** turns the numeric operations (moves, jumps, arithmetic, comparisons, vectors) into SSE machine code.
- The rest (calls, events, strings, arrays) goes back to the interpreter one operation at a time: the behavior is identical.
- In an arithmetic loop of 40 thousand turns: 3.2 ms interpreted, 0.4 ms native (8×).
- `REALENGINE_JIT=0` turns it off (to compare or to isolate a problem).
--- es
```diagram bars ms
Interpretado: 3.16
Nativo (JIT): 0.40
```
- Los Grafos y scripts se compilan a un **bytecode por registros**; en la primera ejecución, un **JIT x64** convierte las operaciones numéricas (movimientos, saltos, aritmética, comparaciones, vectores) en código de máquina SSE.
- El resto (llamadas, eventos, cadenas, arrays) vuelve al intérprete una operación a la vez: el comportamiento es idéntico.
- En un bucle aritmético de 40 mil vueltas: 3,2 ms interpretado, 0,4 ms nativo (8×).
- `REALENGINE_JIT=0` lo desactiva (para comparar o aislar un problema).
