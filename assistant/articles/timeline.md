id: timeline
kind: recipe
title: Timeline: curvas no tempo nos Blueprints | Timeline: curves over time in Blueprints
keys: timeline, linha do tempo, curva, curve, animar valor, animate a value, fade, piscar, flicker, porta abrindo, door opening, play from start, reverse, reverse from end, set new time, update, finished, direction, trilha, track, float track, vector track, color track, event track, chave, keyframe, autoplay, loop, play rate, interpolacao, interpolation, lerp no tempo, acender devagar, apagar devagar
--- pt
O nó **Timeline** é o da Unreal: curvas que tocam no tempo dentro de um Blueprint. Clique com o botão direito no grafo, procure "Timeline", e uma Timeline nova já abre no editor dela (dois cliques no nó abrem de novo).
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
The **Timeline** node is Unreal's: curves played over time inside a Blueprint. Right-click the graph, search "Timeline", and a new Timeline opens in its editor (double-click the node to open it again).
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
