id: organize-nodes
kind: howto
title: Organizar os nós de qualquer grafo | Organize the nodes of any graph
keys: organizar nos, organize nodes, arrumar nos, tidy nodes, reorganizar, rearrange, layout automatico, auto layout, auto arrange, nos baguncados, messy nodes, alinhar nos, align nodes, straighten, blueprint organizado, material organizado, grafo organizado, q organizar, node arrange
--- pt
```diagram keys
Q: Organizar Nós | Selecionar 2+: só eles | Ctrl+Z: desfaz
```
## Organizar Nós
- Em qualquer editor de nós (**Material**, **Blueprint**, **Anim Graph**, **Sound Cue**, **Diálogo**, **PCG**, **Behavior Tree**), aperte **Q** com o mouse sobre o grafo, ou use **Organizar Nós** no menu do botão direito.
- Os nós vão para **colunas na direção dos fios**: a Material Output e o fim das cadeias de eventos à direita, cada nó logo à esquerda de quem ele alimenta, sem nada sobreposto.
- Com **dois ou mais nós selecionados**, só eles são arrumados; o resto fica onde está.
- O **Diálogo** vai do Start para a direita, uma resposta embaixo da outra. A **Behavior Tree** desce da raiz e mantém os filhos na ordem em que rodam, da esquerda para a direita.
- **Ctrl+Z** volta como estava.

## Rolar pela borda
Arrastando um nó ou um fio até perto da borda do grafo (ou além dela), a vista **rola sozinha** para esse lado, como no Blender; o nó continua sob o mouse. Quanto mais perto da borda, mais rápido.
--- en
```diagram keys
Q: Organize Nodes | Select 2+: only them | Ctrl+Z: undo
```
## Organize Nodes
- In any node editor (**Material**, **Blueprint**, **Anim Graph**, **Sound Cue**, **Dialogue**, **PCG**, **Behavior Tree**), press **Q** with the mouse over the graph, or use **Organize Nodes** in the right-click menu.
- The nodes go into **columns along their wires**: the Material Output and the ends of the event chains on the right, each node just left of what it feeds, nothing overlapping.
- With **two or more nodes selected**, only they are arranged; the rest stays where it is.
- A **Dialogue** goes from its Start to the right, one answer under the other. A **Behavior Tree** goes down from its root and keeps the children in the order they run, left to right.
- **Ctrl+Z** puts it back.

## Scroll at the edge
Dragging a node or a wire near the graph's edge (or past it), the view **scrolls by itself** that way, as in Blender; the node stays under the mouse. The closer to the edge, the faster.
