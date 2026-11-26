id: content-filters
kind: howto
title: Filtros do Content Browser | Content Browser filters | Filtros del Explorador de contenidos
keys: filtros, filtro, filters, filter, filtrar, filtrar assets, filtrar por tipo, filter by type, tipo de asset, asset type, favoritos, favorito, favorites, favorite, estrela, star, adicionar aos favoritos, add to favorites, recentes, abertos recentemente, recently opened, recent, modificados, modified, não salvos, nao salvos, unsaved, em uso no nível, em uso no nivel, in use in level, usados no nível, assets do nível, etiqueta, chip, chips, limpar filtros, reset filters, só texturas, so texturas, só materiais, so materiais, só blueprints, mostrar só, content browser, content drawer, grafo, grafos, graph, graphs
--- pt
O botão **Filtros** fica à esquerda da busca do **Content Browser** (como o Filters da Unreal). Ele abre um menu com:
- **Outros Filtros**: **Favoritos**, **Abertos Recentemente** (os últimos 32 abertos), **Modificados** (Grafo alterado e não salvo, o nível com mudanças, ou arquivo novo/alterado no Git) e **Em Uso no Nível** (o que os atores do nível aberto usam: malhas, materiais, classes, animações, sons, partículas...).
- **Tipos de Asset**: Classe de Grafo, Widget Graph, Material, Material Instance, Static Mesh, Skeletal Mesh, Textura, Som, Nível, Animação, Sistema de Partículas, Level Sequence, Data Asset; em **Mais Tipos**, todos os outros (Estrutura, Enumeração, IA, Entrada, Script, IK Rig, Física, Veículo, Diálogo, Missão, Camera Shake, PCG, Arquivo de Modelo, Outro Arquivo).
- Cada filtro escolhido vira uma **etiqueta colorida** embaixo da busca, na cor da família do ícone. **Clique** liga ou desliga; **clique direito** remove. **Limpar Filtros** tira todos.
- Como se combinam: os **tipos ligados se somam** (Textura + Material mostra os dois); os **Outros Filtros e a busca restringem** (Favoritos + Textura = só as texturas favoritas). Com um filtro ligado a lista mostra a pasta atual **e as de dentro**, sem as pastas.
- **Favoritos**: clique direito num asset > **Adicionar aos Favoritos** (vale para a seleção toda); uma estrela aparece no bloco.
- As etiquetas, quais estão ligadas, os favoritos e os recentes ficam salvos nas suas preferências (editor.ini).

```diagram steps
Filtros | escolha Textura e Favoritos | etiquetas embaixo da busca | clique liga/desliga | clique direito remove
```
```diagram bars ms
Ler a pasta com 1009 assets: 1.04
A mesma com o filtro Textura: 0.86
```
--- en
The **Filters** button sits left of the **Content Browser**'s search (like Unreal's Filters). It opens a menu with:
- **Other Filters**: **Favorites**, **Recently Opened** (the last 32 opened), **Modified** (a Graph changed and not saved, the level with changes, or a file new/changed in Git) and **In Use in Level** (what the open level's actors use: meshes, materials, classes, animations, sounds, particles...).
- **Asset Types**: Graph Class, Widget Graph, Material, Material Instance, Static Mesh, Skeletal Mesh, Texture, Sound, Level, Animation, Particle System, Level Sequence, Data Asset; under **More Types**, all the others (Structure, Enumeration, AI, Input, Script, IK Rig, Physics, Vehicle, Dialogue, Quest, Camera Shake, PCG, Model File, Other File).
- Each filter chosen becomes a **colored chip** under the search, in its icon family's color. **Click** turns it on or off; **right click** removes it. **Reset Filters** removes them all.
- How they combine: the **types that are on add up** (Texture + Material shows both); the **Other Filters and the search narrow them** (Favorites + Texture = only the favorite textures). With a filter on, the list shows the current folder **and the ones inside it**, without the folders.
- **Favorites**: right click an asset > **Add to Favorites** (the whole selection); a star shows on its tile.
- The chips, which are on, the favorites and the recent assets are kept in your preferences (editor.ini).

```diagram steps
Filters | pick Texture and Favorites | chips under the search | click: on/off | right click: remove
```
```diagram bars ms
Reading a folder of 1009 assets: 1.04
The same with the Texture filter: 0.86
```
--- es
El botón **Filters** está a la izquierda de la búsqueda del **Explorador de contenidos** (como los Filters de Unreal). Abre un menú con:
- **Other Filters**: **Favorites**, **Recently Opened** (los últimos 32 abiertos), **Modified** (un Grafo modificado y sin guardar, el nivel con cambios, o un archivo nuevo/modificado en Git) e **In Use in Level** (lo que usan los actores del nivel abierto: mallas, materiales, clases, animaciones, sonidos, partículas...).
- **Asset Types**: Clase de Grafo, Widget Graph, Material, Material Instance, Static Mesh, Skeletal Mesh, Texture, Sound, Level, Animation, Particle System, Level Sequence, Data Asset; en **More Types**, todos los demás (Structure, Enumeration, AI, Input, Script, IK Rig, Physics, Vehicle, Dialogue, Quest, Camera Shake, PCG, Model File, Other File).
- Cada filtro elegido se convierte en un **chip de color** debajo de la búsqueda, con el color de la familia de su icono. **Clic** lo activa o desactiva; **clic derecho** lo quita. **Reset Filters** los quita todos.
- Cómo se combinan: los **tipos activados se suman** (Texture + Material muestra ambos); los **Other Filters y la búsqueda los restringen** (Favorites + Texture = solo las texturas favoritas). Con un filtro activado, la lista muestra la carpeta actual **y las que contiene**, sin las carpetas.
- **Favorites**: clic derecho en un asset > **Add to Favorites** (toda la selección); aparece una estrella en su mosaico.
- Los chips, cuáles están activados, los favoritos y los assets recientes se guardan en tus preferencias (editor.ini).

```diagram steps
Filters | elige Texture y Favorites | chips debajo de la búsqueda | clic: activar/desactivar | clic derecho: quitar
```
```diagram bars ms
Leer una carpeta de 1009 assets: 1.04
Lo mismo con el filtro Texture: 0.86
```
