id: asset-picker
kind: howto
title: Seletor de assets e Criar a partir da seleção | Asset picker and Create from the selection | Selector de assets
keys: seletor de assets, seletor, asset picker, escolher asset, escolher modelo, escolher malha, escolher material, escolher textura, escolher classe pai, pick parent class, classe pai, parent class, pick asset, campo de asset, prévia, previa, miniatura, miniaturas, thumbnail, só prévia, so previa, só nome, so nome, prévia e nome, tamanho da prévia, pequeno, médio, grande, criar a partir da seleção, create from selection, clique direito, right click, criar ik rig, create ik rig, criar physics asset, criar ragdoll, criar anim blueprint, criar material instance, material instance, instância de material, instancia de material, criar material da textura, light function da textura, criar montage, criar blend space, gerar lods, generate lods, blueprint da malha, ator com física, tecido da malha, veículo da malha, grafo, grafos, graph, graphs
--- pt
A **janela de seleção de asset** (como o Asset Picker da Unreal) abre em todo campo de asset do **Details** (Static Mesh, Material, Light Function, Skeletal Mesh, Anim Class, Particle System, Vehicle, Decal) e em toda criação que precisa de um asset de base: **Adicionar > Classe de Grafo...** (Escolher Classe Pai), **Animação > Animation Graph... / Anim Montage... / Blend Space...**, **IK Rig e Física > IK Rig... / Physics Asset... / Ragdoll...**.
- **Buscar**: cada palavra no nome ou na pasta ("mann sk" acha SK_Mannequin). **Pasta**: só a pasta e as de dentro. **Tipos**: um clique mostra só aquele tipo, Ctrl+clique liga ou desliga.
- O **olho** no canto: **Só prévia**, **Só nome** (lista com tipo e pasta) ou **Prévia e nome**; tamanho **Pequeno / Médio / Grande**. Fica lembrado nas preferências.
- Duplo clique, Enter ou **Selecionar** escolhe; **Nenhum** esvazia o campo; Esc cancela. Arrastar um asset do Content Browser para o campo também vale.
- **Clique direito num asset > Criar**: Skeletal Mesh → IK Rig, Physics Asset, Ragdoll, Anim Graph, Esqueleto; Static Mesh → Classe de Grafo, Ator com Física, Gerar LODs, Tecido, Veículo; Material → **Material Instance**; Textura → Material, Light Function; Animação → Anim Montage, Blend Space. O asset novo nasce na mesma pasta, já renomeando.
- A Material Instance não está mais no menu Adicionar: ela nasce do clique direito no material.

```diagram steps
Clique direito no asset | Criar | IK Rig (ou Material Instance...) | digite o nome | o editor abre
```
```diagram bars ms
Ler 1031 assets: 2.0
Abrir (ler + 1º quadro): 3.7
Um quadro aberto (CPU): 0.6
```
--- en
The **asset picker** (like Unreal's Asset Picker) opens on every asset field of the **Details** (Static Mesh, Material, Light Function, Skeletal Mesh, Anim Class, Particle System, Vehicle, Decal) and on every creation that needs a base asset: **Add > Graph Class...** (Pick Parent Class), **Animation > Animation Graph... / Anim Montage... / Blend Space...**, **IK Rig and Physics > IK Rig... / Physics Asset... / Ragdoll...**.
- **Search**: every word in the name or the folder ("mann sk" finds SK_Mannequin). **Folder**: only that folder and the ones under it. **Types**: a click shows only that type, Ctrl+click turns one on or off.
- The **eye** in the corner: **Preview only**, **Name only** (a list with the type and folder) or **Preview and name**; size **Small / Medium / Large**. It is kept in the preferences.
- Double-click, Enter or **Select** picks; **None** empties the field; Esc cancels. Dragging an asset from the Content Browser onto the field works too.
- **Right click an asset > Create**: Skeletal Mesh → IK Rig, Physics Asset, Ragdoll, Anim Graph, Skeleton; Static Mesh → Graph Class, Physics Actor, Generate LODs, Cloth, Vehicle; Material → **Material Instance**; Texture → Material, Light Function; Animation → Anim Montage, Blend Space. The new asset is made in the same folder, being renamed.
- Material Instance is no longer in the Add menu: it is made from the material's right click.

```diagram steps
Right click the asset | Create | IK Rig (or Material Instance...) | type its name | its editor opens
```
```diagram bars ms
Reading 1031 assets: 2.0
Opening (read + first frame): 3.7
A frame while open (CPU): 0.6
```
--- es
El **selector de assets** (como el Asset Picker de Unreal) se abre en cada campo de asset de **Detalles** (Static Mesh, Material, Light Function, Skeletal Mesh, Anim Class, Particle System, Vehicle, Decal) y en cada creación que necesita un asset base: **Añadir > Clase de Grafo...** (Pick Parent Class), **Animación > Grafo de animación... / Anim Montage... / Blend Space...**, **IK Rig y Física > IK Rig... / Physics Asset... / Ragdoll...**.
- **Búsqueda**: cada palabra en el nombre o en la carpeta ("mann sk" encuentra SK_Mannequin). **Carpeta**: solo esa carpeta y las de dentro. **Tipos**: un clic muestra solo ese tipo, Ctrl+clic activa o desactiva uno.
- El **ojo** de la esquina: **Solo vista previa**, **Solo nombre** (una lista con el tipo y la carpeta) o **Vista previa y nombre**; tamaño **Pequeño / Mediano / Grande**. Se guarda en las preferencias.
- Doble clic, Enter o **Seleccionar** elige; **Ninguno** vacía el campo; Esc cancela. Arrastrar un asset del Explorador de contenidos hasta el campo también funciona.
- **Clic derecho en un asset > Crear**: Skeletal Mesh → IK Rig, Physics Asset, Ragdoll, Grafo de animación, Skeleton; Static Mesh → Clase de Grafo, Physics Actor, Generar LODs, Tela, Vehículo; Material → **Material Instance**; Textura → Material, Light Function; Animación → Anim Montage, Blend Space. El nuevo asset se crea en la misma carpeta, ya en modo de renombrar.
- Material Instance ya no está en el menú Añadir: se crea con el clic derecho del material.

```diagram steps
Clic derecho en el asset | Crear | IK Rig (o Material Instance...) | escribe su nombre | se abre su editor
```
```diagram bars ms
Lectura de 1031 assets: 2.0
Apertura (lectura + primer cuadro): 3.7
Un cuadro con ella abierta (CPU): 0.6
```
