id: graph-style
kind: howto
title: Estilo dos grafos (Órbita) | Graph style (Orbit)
keys: estilo do grafo, graph style, estilo dos nós, node style, estilo dos fios, wire style, órbita, orbita, orbit, cartão, cartao, card, pino, pinos, pin, pins, cor do pino, pin color, forma do pino, pin shape, array, set, map, fio, fios, wire, wires, degradê, degrade, gradient, fundo de pontos, dot grid, nó latente, latent node, clássico, classic, estilo de nodos, estilo de cables, forma del pin
--- pt
```diagram steps
Editar > Preferências do Editor > Estilo do Grafo | Estilo dos nós: Órbita (padrão) ou um dos 5 antigos | Estilo dos fios: Degradê (padrão) ou outro | Os grafos abertos mudam na hora
```
## O estilo Órbita
Todos os grafos da engine (Blueprint, Anim Graph, Material, Behavior Tree, Sound Cue, Diálogo e os emissores de partículas) usam o mesmo visual:
- **nós como cartões** de cantos arredondados, com uma faixa no cabeçalho na cor da família e o ícone dela: **Evento** em laranja Ignição (um raio), **Função** em azul, **Pura** em verde-água (um losango), **Latente** em violeta com um **relógio** (Delay, Timeline, nós que terminam depois), **Macro** em cinza-azulado, **Fluxo** (Branch, Sequence) em branco gelo, **Variável** na cor do tipo;
- **selecionado**: um halo na cor do acento; **erro**: contorno vermelho e um selo com `!` no canto;
- **pinos por família na cor e na forma**: valor simples num círculo, **Array** numa barra de três segmentos, **Set** num anel duplo, **Map** em dois círculos ligados (a chave e o valor), **Class** num anel vazado, execução numa seta. A forma ajuda quem tem daltonismo;
- **fios em degradê**, da cor da saída para a da entrada;
- **fundo de pontos** em vez de quadriculado, com a grade maior surgindo quando você aproxima.

## Voltar ao estilo antigo
Em [Editar > Preferências do Editor](open:prefs) > **Estilo do Grafo**, a galeria mostra cada estilo de nó e de fio ao vivo. Os 5 de antes (**Clássico**, Plano, Mínimo, Vidro, Neon) e os fios Curva, Reta, Circuito, Brilho e Fluxo continuam lá. A escolha fica salva.
--- en
```diagram steps
Edit > Editor Preferences > Graph Style | Node style: Orbit (default) or one of the 5 older ones | Wire style: Gradient (default) or another | The open graphs change at once
```
## The Orbit style
Every graph of the engine (Blueprint, Anim Graph, Material, Behavior Tree, Sound Cue, Dialogue and the particle emitters) has the same look:
- **nodes as cards** with round corners, a header band in the family's color and its icon: **Event** in Ignition orange (a bolt), **Function** in blue, **Pure** in teal (a diamond), **Latent** in violet with a **clock** (Delay, Timeline, nodes that finish later), **Macro** in blue-gray, **Flow** (Branch, Sequence) in ice white, **Variable** in its type's color;
- **selected**: a halo in the accent's color; **error**: a red outline and a badge with `!` on the corner;
- **pins by family in color and in shape**: a plain value a circle, an **Array** a bar of three segments, a **Set** a double ring, a **Map** two linked circles (the key and the value), a **Class** a hollow ring, the execution an arrow. The shape helps with color blindness;
- **wires in a gradient**, from the output's color to the input's;
- **a background of dots** instead of a grid, the bigger grid surfacing as you zoom in.

## Back to the older style
In [Edit > Editor Preferences](open:prefs) > **Graph Style**, the gallery shows each node and wire style live. The 5 from before (**Classic**, Flat, Minimal, Glass, Neon) and the Curve, Straight, Circuit, Glow and Flow wires are still there. The choice is saved.
