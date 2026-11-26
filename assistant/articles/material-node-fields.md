id: material-node-fields
kind: howto
title: Material Editor: editar os valores direto no nó | Material Editor: edit values right on the node | Material Editor: editar los valores directamente en el nodo
keys: material editor, editor de material, valor no no, value on node, editar no no, edit on node, campo, field, arrastar valor, drag value, slider, barra, fator, factor, cor no no, color on node, seletor de cor, color picker, lista do no, node dropdown, distribuicao, distribution, metodo, method, como no blender, like blender, como no tlender, like tlender, sem detalhes, without details, mudar valor rapido, change value fast, numero, number
--- pt
```diagram steps
Aproxime o zoom | Arraste o número no nó | Clique para digitar | O material atualiza sozinho
```
## Os valores ficam no próprio nó (como no Blender e no Tlender)
- **Números**: arraste para os lados para mudar; um clique sem arrastar abre para **digitar** o valor (Enter confirma). O nome da entrada fica dentro do campo.
- **Fatores (0 a 1)**, como Metálico e Alfa: a barra se enche até o valor; arraste para escolher.
- **Cores**: clique na amostra ao lado da entrada (ou na faixa do nó RGB) para abrir o seletor de cor.
- **Vetores**: os três números X Y Z ao lado do nome.
- **Configurações do nó** (Distribuição, Método da subsuperfície, operação do Math...): a lista fica logo abaixo das saídas; caixas de seleção também.
- Nós **Value** e **RGB**: o valor ou a cor no corpo do nó.

Uma entrada **ligada** não mostra campo (o valor vem do link). Os campos aparecem com zoom de 80% ou mais; mais longe, o nó mostra o valor como texto.
Mexer num campo seleciona o nó; o painel de **Detalhes** continua com todos os campos e mostra a mesma coisa.

Enquanto você arrasta, o material é refeito sem compressão (rápido e sem trocar as texturas); quando você para, ele é comprimido de novo.
--- en
```diagram steps
Zoom in | Drag the number on the node | Click to type | The material updates by itself
```
## The values live on the node itself (as in Blender and the Tlender)
- **Numbers**: drag sideways to change them; a click without dragging opens the value for **typing** (Enter confirms). The input's name sits inside the field.
- **Factors (0 to 1)**, such as Metallic and Alpha: the bar fills up to the value; drag to pick.
- **Colors**: click the swatch next to the input (or the RGB node's strip) to open the color picker.
- **Vectors**: the three numbers X Y Z next to the name.
- **Node settings** (Distribution, Subsurface Method, the Math operation...): the list sits right under the outputs; checkboxes too.
- **Value** and **RGB** nodes: the value or the color in the node's body.

A **linked** input shows no field (its value comes from the link). The fields show at 80% zoom or more; farther out, the node shows the value as text.
Touching a field selects its node; the **Details** panel keeps every field and shows the same values.

While you drag, the material is rebaked without compression (fast, the same textures reused); once you stop, it's compressed again.
--- es
```diagram steps
Acércate con el zoom | Arrastra el número en el nodo | Haz clic para escribir | El material se actualiza solo
```
## Los valores viven en el propio nodo (como en Blender y el Tlender)
- **Números**: arrastra hacia los lados para cambiarlos; un clic sin arrastrar abre el valor para **escribir** (Enter confirma). El nombre de la entrada queda dentro del campo.
- **Factores (0 a 1)**, como Metallic y Alpha: la barra se llena hasta el valor; arrastra para elegir.
- **Colores**: haz clic en la muestra junto a la entrada (o en la franja del nodo RGB) para abrir el selector de color.
- **Vectores**: los tres números X Y Z junto al nombre.
- **Ajustes del nodo** (Distribution, Subsurface Method, la operación Math...): la lista queda justo debajo de las salidas; las casillas también.
- Nodos **Value** y **RGB**: el valor o el color en el cuerpo del nodo.

Una entrada **enlazada** no muestra campo (su valor viene del enlace). Los campos aparecen con un zoom del 80 % o más; más lejos, el nodo muestra el valor como texto.
Tocar un campo selecciona su nodo; el panel de **Detalles** mantiene todos los campos y muestra los mismos valores.

Mientras arrastras, el material se vuelve a hornear sin compresión (rápido, reutilizando las mismas texturas); al soltar, se comprime de nuevo.
