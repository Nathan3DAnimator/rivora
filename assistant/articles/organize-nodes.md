id: organize-nodes
kind: howto
title: Organizar os nós de qualquer grafo | Organize the nodes of any graph | Organizar nodos
keys: organizar nos, organize nodes, arrumar nos, tidy nodes, reorganizar, rearrange, layout automatico, auto layout, auto arrange, nos baguncados, messy nodes, alinhar nos, align nodes, straighten, blueprint organizado, material organizado, grafo organizado, q organizar, node arrange
--- pt
```diagram keys
Q: Organizar Nós | Selecionar 2+: só eles | Ctrl+Z: desfaz
```
## Organizar Nós
- Em qualquer editor de nós (**Material**, **Grafo**, **Anim Graph**, **Sound Cue**, **Diálogo**, **PCG**, **Behavior Tree**), aperte **Q** com o mouse sobre o grafo, ou use **Organizar Nós** no menu do botão direito.
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
- In any node editor (**Material**, **Graph**, **Anim Graph**, **Sound Cue**, **Dialogue**, **PCG**, **Behavior Tree**), press **Q** with the mouse over the graph, or use **Organize Nodes** in the right-click menu.
- The nodes go into **columns along their wires**: the Material Output and the ends of the event chains on the right, each node just left of what it feeds, nothing overlapping.
- With **two or more nodes selected**, only they are arranged; the rest stays where it is.
- A **Dialogue** goes from its Start to the right, one answer under the other. A **Behavior Tree** goes down from its root and keeps the children in the order they run, left to right.
- **Ctrl+Z** puts it back.

## Scroll at the edge
Dragging a node or a wire near the graph's edge (or past it), the view **scrolls by itself** that way, as in Blender; the node stays under the mouse. The closer to the edge, the faster.
--- es
```diagram keys
Q: Organizar nodos | Selecciona 2+: solo ellos | Ctrl+Z: deshacer
```
## Organizar nodos
- En cualquier editor de nodos (**Material**, **Grafo**, **Anim Graph**, **Sound Cue**, **Dialogue**, **PCG**, **Behavior Tree**), pulsa **Q** con el ratón sobre el grafo, o usa **Organizar nodos** en el menú del clic derecho.
- Los nodos pasan a **columnas siguiendo sus cables**: el Material Output y los extremos de las cadenas de eventos a la derecha, cada nodo justo a la izquierda de lo que alimenta, sin que nada se superponga.
- Con **dos o más nodos seleccionados**, solo ellos se ordenan; el resto se queda donde está.
- Un **Dialogue** va desde su Start hacia la derecha, una respuesta debajo de la otra. Un **Behavior Tree** baja desde su raíz y mantiene a los hijos en el orden en que se ejecutan, de izquierda a derecha.
- **Ctrl+Z** lo deja como estaba.

## Desplazamiento en el borde
Al arrastrar un nodo o un cable cerca del borde del grafo (o más allá), la vista se **desplaza sola** hacia ese lado, como en Blender; el nodo se queda bajo el ratón. Cuanto más cerca del borde, más rápido.
