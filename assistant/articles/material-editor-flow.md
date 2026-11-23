id: material-editor-flow
kind: howto
title: Material Editor: criar nós soltando o link e o painel de Detalhes | Material Editor: make nodes by dropping a link, and the Details panel
keys: material editor, editor de material, soltar link, drop link, arrastar link, drag link, criar no, create node, menu de nos, node menu, puxar da entrada, pull from input, detalhes, details, abas, tabs, buscar detalhes, search details, no selecionado, selected node, ligado a, linked from, lento, slow, atraso, lag, arrastar no, drag node
--- pt
```diagram steps
Arraste de um pino | Solte no vazio | Digite (ex.: mix) e Enter | O nó nasce ligado
```
## Criar nós pelo link (como no Tlender e nas Blueprints)
- Arraste um link de uma **saída** e solte no vazio: abre o menu só com os nós que recebem aquele tipo. Escolha um (ou digite e **Enter**) e ele nasce **já ligado**.
- Funciona ao contrário: arraste de uma **entrada livre** e solte no vazio para criar o nó que a alimenta (ex.: Image Texture na Cor Base).
- Soltar sobre o corpo de um nó liga na primeira entrada que serve. Um link tirado de uma entrada e solto no vazio só se desfaz.

## O painel de Detalhes
- **Buscar nos detalhes** filtra as seções e entradas pelo nome, em português ou inglês.
- Aba **Material**: as configurações do material em seções; as que estão em uso já abrem.
- Aba **Nó**: o nó selecionado. Cada entrada ligada mostra **de qual nó vem**: um clique vai até ele.
- As abas seguem a seleção: clicar num nó abre a aba Nó; clicar no vazio volta para Material.

Os nós e os links acompanham o mouse no mesmo quadro (sem atraso), também com o editor numa janela separada.
--- en
```diagram steps
Drag from a pin | Drop on empty space | Type (e.g. mix) and Enter | The node comes in linked
```
## Making nodes from a link (as in the Tlender and the Blueprints)
- Drag a link from an **output** and drop it on empty space: the menu opens with only the nodes that take that type. Pick one (or type and press **Enter**) and it comes in **already linked**.
- It works backwards too: drag from a **free input** and drop it on empty space to make the node that feeds it (e.g. an Image Texture into the Base Color).
- Dropping on a node's body links its first input that fits. A link taken off an input and dropped on nothing is just removed.

## The Details panel
- **Search details** filters the sections and inputs by name, in English or Portuguese.
- **Material** tab: the material's settings in sections; the ones in use start open.
- **Node** tab: the selected node. Each linked input shows **which node it comes from**: a click goes there.
- The tabs follow the selection: clicking a node opens the Node tab; clicking empty space goes back to Material.

The nodes and links follow the mouse in the same frame (no lag), also with the editor in a separate window.
