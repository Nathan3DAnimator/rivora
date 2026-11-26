id: split-struct-pin
kind: howto
title: Dividir o pino (Split Struct Pin) | Split Struct Pin | Dividir el pin (Split Struct Pin)
keys: split, split struct pin, dividir pino, dividir o pino, recombinar, recombine, recombine struct pin, pino da estrutura, struct pin, transform pin, pino transform, location rotation scale, x y z, separar o vetor, break vector, make vector, quebrar transform, desfazer split, ctrl+z blueprint, desfazer no blueprint, undo blueprint, grafo, grafos, graph, graphs
--- pt
Um pino com várias partes (**Transform**, **Vector**, **Rotator**, **Linear Color**, uma **Structure** sua) pode ser **dividido** no próprio nó, como na Unreal: clique direito no pino > **Split Struct Pin**.

```diagram steps
Clique direito em New Transform | Split Struct Pin | Location, Rotation, Scale | clique direito em Location | Split Struct Pin | X, Y, Z
```
- **Transform** → Location, Rotation, Scale. **Vector** → X, Y, Z. **Rotator** → Pitch, Yaw, Roll. **Linear Color** → R, G, B, A. **Structure** → os campos dela.
- Uma parte pode ser **dividida de novo**: Transform → Location → X, Y, Z.
- Vale para entradas e saídas, em qualquer nó, variável, evento e função.
- Os fios que estavam no pino somem ao dividir (como na Unreal). Nas partes que ficarem sem fio vale o valor digitado (Scale começa em 1).
- **Recombine Struct Pin** (clique direito numa parte) junta de volta: numa parte dividida de novo, junta só aquela parte; numa parte comum, o pino inteiro.
- **Ctrl+Z** desfaz e **Ctrl+Y** refaz o split, o recombine, o Delete e o Organize Nodes do grafo.
- Não custa nada a mais quando roda: o compilador monta e lê as partes direto nos registradores, sem os nós Make/Break.
- Fica salvo no Grafo; os Grafos antigos abrem iguais.
--- en
A pin with several parts (**Transform**, **Vector**, **Rotator**, **Linear Color**, a **Structure** of yours) can be **split** on the node itself, like Unreal: right click the pin > **Split Struct Pin**.

```diagram steps
Right click New Transform | Split Struct Pin | Location, Rotation, Scale | right click Location | Split Struct Pin | X, Y, Z
```
- **Transform** → Location, Rotation, Scale. **Vector** → X, Y, Z. **Rotator** → Pitch, Yaw, Roll. **Linear Color** → R, G, B, A. **Structure** → its fields.
- A part can be **split again**: Transform → Location → X, Y, Z.
- It works on inputs and outputs, on any node, variable, event and function.
- The wires on the pin go when it is split (as in Unreal). A part left unwired takes the value typed in it (Scale starts at 1).
- **Recombine Struct Pin** (right click a part) joins them back: on a part split again, only that part; on a plain part, the whole pin.
- **Ctrl+Z** undoes and **Ctrl+Y** redoes a split, a recombine, a Delete and Organize Nodes in the graph.
- It costs nothing more when it runs: the compiler builds and reads the parts right in the registers, without Make/Break nodes.
- Saved in the Graph; older Graphs open the same.
--- es
Un pin con varias partes (**Transform**, **Vector**, **Rotator**, **Linear Color**, una **Structure** tuya) se puede **dividir** en el propio nodo, como en Unreal: clic derecho en el pin > **Split Struct Pin**.

```diagram steps
Clic derecho en New Transform | Split Struct Pin | Location, Rotation, Scale | clic derecho en Location | Split Struct Pin | X, Y, Z
```
- **Transform** → Location, Rotation, Scale. **Vector** → X, Y, Z. **Rotator** → Pitch, Yaw, Roll. **Linear Color** → R, G, B, A. **Structure** → sus campos.
- Una parte se puede **dividir de nuevo**: Transform → Location → X, Y, Z.
- Funciona en entradas y salidas, en cualquier nodo, variable, evento y función.
- Los cables del pin se van cuando se divide (como en Unreal). Una parte sin cable toma el valor escrito en ella (Scale empieza en 1).
- **Recombine Struct Pin** (clic derecho en una parte) los vuelve a unir: en una parte dividida de nuevo, solo esa parte; en una parte simple, todo el pin.
- **Ctrl+Z** deshace y **Ctrl+Y** rehace una división, una recombinación, un Delete y Organize Nodes en el grafo.
- No cuesta nada más al ejecutarse: el compilador construye y lee las partes directamente en los registros, sin nodos Make/Break.
- Se guarda en el Grafo; los Grafos más antiguos se abren igual.
