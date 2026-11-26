id: timeline
kind: recipe
title: Timeline: curvas no tempo nos Grafos | Timeline: curves over time in Graphs | Timeline: curvas en el tiempo en los Grafos
keys: timeline, linha do tempo, curva, curve, animar valor, animate a value, fade, piscar, flicker, porta abrindo, door opening, play from start, reverse, reverse from end, set new time, update, finished, direction, trilha, track, float track, vector track, color track, event track, chave, keyframe, autoplay, loop, play rate, interpolacao, interpolation, lerp no tempo, acender devagar, apagar devagar
--- pt
O nó **Timeline** é o da Unreal: curvas que tocam no tempo dentro de um Grafo. Clique com o botão direito no grafo, procure "Timeline", e uma Timeline nova já abre no editor dela (dois cliques no nó abrem de novo).
- **Trilhas**: **Float** (um número: fade, alpha, altura), **Vector** (posição, escala), **Linear Color** (a cor de uma luz) e **Event** (dispara o pino quando o tempo passa cada chave). Cada trilha vira um pino de saída do nó.
- **Chaves**: Shift+clique na curva adiciona uma, arrastar move, Delete apaga, botão direito escolhe a interpolação (**Auto** suave, **Linear**, **Constant** em degrau). Os campos Time e Value editam a escolhida. Roda: zoom; Ctrl+roda: zoom nos valores; arrastar com o direito: mover a vista.
- **Configuração**: Length (ou **Use Last Keyframe**), **AutoPlay** (toca sozinha quando o ator começa), **Loop**, **Play Rate**.
- **Entradas**: Play, Play from Start, Stop, Reverse, Reverse from End, Set New Time (New Time). **Saídas**: Update (todo quadro em que toca, com os valores das trilhas), Finished (ao chegar numa ponta), Direction (0 para frente, 1 para trás).
- Cada ator tem o próprio tempo; nada é alocado enquanto toca (uma curva de 64 chaves: ~46 ns por leitura).
- Combina com **Set Lighting Level Blend** (acender um Lighting Level devagar, fazer piscar), Set Light Intensity, Set Relative Location (portas, elevadores), materiais.

```diagram steps
Grafo > Timeline | dois cliques: trilha Float "Alpha" 0 -> 1 | Update -> Set Lighting Level Blend (Alpha) | Play from Start
```
```blueprint
Event Key L -> Timeline ( Play from Start )
Timeline.Update -> Set Lighting Level Blend ( Level Name = "Lighting_Night", Blend = Timeline.Alpha )
Event Key K -> Timeline ( Reverse )
```
--- en
The **Timeline** node is Unreal's: curves played over time inside a Graph. Right-click the graph, search "Timeline", and a new Timeline opens in its editor (double-click the node to open it again).
- **Tracks**: **Float** (a number: a fade, an alpha, a height), **Vector** (a position, a scale), **Linear Color** (a light's color) and **Event** (fires its pin when the time passes each key). Each track is an output pin of the node.
- **Keys**: Shift+click on the curve adds one, drag moves it, Delete removes it, right-click picks its interpolation (**Auto** smooth, **Linear**, **Constant** step). The Time and Value fields edit the picked one. Wheel: zoom; Ctrl+wheel: zoom the values; right drag: pan.
- **Settings**: Length (or **Use Last Keyframe**), **AutoPlay** (plays by itself when the actor begins), **Loop**, **Play Rate**.
- **Inputs**: Play, Play from Start, Stop, Reverse, Reverse from End, Set New Time (New Time). **Outputs**: Update (every frame it plays, with the tracks' values), Finished (at an end), Direction (0 forward, 1 backward).
- Each actor keeps its own time; nothing is allocated while it plays (a 64-key curve: ~46 ns a read).
- Goes well with **Set Lighting Level Blend** (fading a Lighting Level in, flickering it), Set Light Intensity, Set Relative Location (doors, lifts), materials.

```diagram steps
Graph > Timeline | double-click: Float track "Alpha" 0 -> 1 | Update -> Set Lighting Level Blend (Alpha) | Play from Start
```
```blueprint
Event Key L -> Timeline ( Play from Start )
Timeline.Update -> Set Lighting Level Blend ( Level Name = "Lighting_Night", Blend = Timeline.Alpha )
Event Key K -> Timeline ( Reverse )
```
--- es
El nodo **Timeline** es el de Unreal: curvas que se reproducen en el tiempo dentro de un Grafo. Haz clic derecho en el grafo, busca "Timeline", y se abre un Timeline nuevo en su editor (doble clic en el nodo para abrirlo de nuevo).
- **Pistas**: **Float** (un número: un fundido, un alfa, una altura), **Vector** (una posición, una escala), **Linear Color** (el color de una luz) y **Event** (dispara su pin cuando el tiempo pasa por cada key). Cada pista es un pin de salida del nodo.
- **Keys**: Shift+clic en la curva añade una, arrastrar la mueve, Delete la quita, clic derecho elige su interpolación (**Auto** suave, **Linear**, **Constant** escalón). Los campos Time y Value editan la elegida. Rueda: zoom; Ctrl+rueda: zoom de los valores; arrastre derecho: desplazar.
- **Ajustes**: Length (o **Use Last Keyframe**), **AutoPlay** (se reproduce solo cuando el actor comienza), **Loop**, **Play Rate**.
- **Entradas**: Play, Play from Start, Stop, Reverse, Reverse from End, Set New Time (New Time). **Salidas**: Update (en cada cuadro que se reproduce, con los valores de las pistas), Finished (en un extremo), Direction (0 hacia adelante, 1 hacia atrás).
- Cada actor guarda su propio tiempo; no se asigna nada mientras se reproduce (una curva de 64 keys: ~46 ns por lectura).
- Va muy bien con **Set Lighting Level Blend** (aparecer un Lighting Level con un fundido, hacerlo parpadear), Set Light Intensity, Set Relative Location (puertas, ascensores), materiales.

```diagram steps
Grafo > Timeline | doble clic: pista Float "Alpha" 0 -> 1 | Update -> Set Lighting Level Blend (Alpha) | Play from Start
```
```blueprint
Event Key L -> Timeline ( Play from Start )
Timeline.Update -> Set Lighting Level Blend ( Level Name = "Lighting_Night", Blend = Timeline.Alpha )
Event Key K -> Timeline ( Reverse )
```
