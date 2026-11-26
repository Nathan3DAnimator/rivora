id: many-skeletal-meshes
kind: howto
title: Milhares de Skeletal Meshes leves | Thousands of light Skeletal Meshes | Miles de Skeletal Meshes ligeros
keys: muitas skeletal mesh, milhares de personagens, thousands of characters, many skeletal meshes, capsulas, capsules, cena pesada, heavy scene, skin, pele, peles paradas, standing skins, pele rigida, rigid skin, um osso, one bone, dormente, dormant, uro, update rate optimization, anim lod, lod de animacao, nanite skeletal, clusters skeletal, enable nanite support, fallback triangle percent, collision complexity, complexidade da colisao, lightmap uv, gerar lightmap
--- pt
A engine deixa leve uma cena com milhares de **Skeletal Meshes** (a pele de cada uma é dobrada na GPU, um "skin"):
- **Pele rígida**: se todos os vértices dependem de um osso só (uma cápsula, uma arma, uma caixa com raiz), a malha é desenhada como uma Static Mesh movida por esse osso, mesmo animada: sem área de pele, sem BLAS própria, com LODs, sombra e raios de malha comum. O log mostra `Rigid skins: N`.
- **Pele parada**: na pose de ligação, desenhada da própria malha (sem dobrar).
- **Dormente**: um ator Movable parado há 2 s sai do conjunto dinâmico de sombra e de raios (o TLAS estático e as sombras em cache) e volta no primeiro movimento ou pose. O log mostra `Dormant dynamic instances: N`.
- **LODs da pele**: feitos na carga (a malha gravada com 1 LOD também); uma malha de topologia difícil usa o simplificador por grade nos LODs distantes.
- **Animação longe atualiza menos** (como o Update Rate Optimizations da Unreal): até 15 m todo quadro, depois a cada 2, 4 e 8 quadros.
- **Nanite na Skeletal Mesh**: uma malha densa (16 384 triângulos ou mais, ou com Nanite em On) ganha o DAG de clusters, que serve também as instâncias animadas: perto, o lado da frente fino e o de trás simples; as esferas dos clusters ganham a folga da pose.
- No **visualizador** de Static ou Skeletal Mesh, em **Nanite Settings**: **Enable Nanite Support** (Auto, On, Off: clusters na GPU), **Fallback Triangle Percent** (o que os raios veem), **Collision Complexity** (Padrão, Simples, Complexa) e **Generate Lightmap UVs**. **Apply Nanite and Collision** salva e já vale para o nível. Logo abaixo: quantos clusters o DAG tem, o LOD dos raios com os triângulos e o erro em cm, e se é uma pele rígida.

```diagram bars ms
Unamed, 5000 cápsulas: CPU antes: 31
Unamed, 5000 cápsulas: CPU agora: 8
```
- Para comparar: `REALENGINE_NO_DORMANT=1`, `REALENGINE_NO_URO=1`.
- Veja também: [Malhas pesadas](article:heavy-meshes), [Skinning](article:skinning).
--- en
The engine keeps a scene with thousands of **Skeletal Meshes** light (each one's skin is bent on the GPU):
- **Rigid skin**: when every vertex hangs on one bone (a capsule, a weapon, a box with a root), the mesh is drawn as a Static Mesh moved by that bone, animated or not: no skin room, no BLAS of its own, a mesh's LODs, shadow and rays. The log says `Rigid skins: N`.
- **Standing skin**: at its bind pose, drawn from the mesh itself (nothing bent).
- **Dormant**: a Movable actor still for 2 s leaves the dynamic set's shadow and rays (the static TLAS and the cached shadows) and comes back at its first move or pose. The log says `Dormant dynamic instances: N`.
- **Skin LODs**: made at the load (a mesh saved with 1 LOD too); a mesh of awkward topology gets its far LODs from the grid simplifier.
- **Far animation updates less** (like Unreal's Update Rate Optimizations): every frame up to 15 m, then every 2, 4 and 8 frames.
- **Nanite on a Skeletal Mesh**: a dense mesh (16 384 triangles or more, or Nanite On) gets the cluster DAG, which serves its animated instances too: near, its front side fine and its back side coarse; the clusters' spheres get the pose's slack.
- In the Static or Skeletal Mesh **viewer**, under **Nanite Settings**: **Enable Nanite Support** (Auto, On, Off: clusters on the GPU), **Fallback Triangle Percent** (what the rays see), **Collision Complexity** (Default, Simple, Complex) and **Generate Lightmap UVs**. **Apply Nanite and Collision** saves them and the level uses them at once. Below them: its DAG's clusters, the rays' LOD with its triangles and error in cm, and whether it is a rigid skin.

```diagram bars ms
Unamed, 5000 capsules: CPU before: 31
Unamed, 5000 capsules: CPU now: 8
```
- To compare: `REALENGINE_NO_DORMANT=1`, `REALENGINE_NO_URO=1`.
- See also: [Heavy meshes](article:heavy-meshes), [Skinning](article:skinning).
--- es
El motor mantiene ligera una escena con miles de **Skeletal Meshes** (el skin de cada uno se dobla en la GPU):
- **Skin rígido**: cuando cada vértice depende de un solo hueso (una cápsula, un arma, una caja con una raíz), la malla se dibuja como un Static Mesh movido por ese hueso, animado o no: sin espacio de skin, sin BLAS propia, con los LODs de una malla, sombra y rayos. El registro dice `Rigid skins: N`.
- **Skin en reposo**: en su pose de ligadura, dibujado a partir de la propia malla (nada se dobla).
- **Dormido**: un actor Movable quieto durante 2 s sale de la sombra y los rayos del conjunto dinámico (el TLAS estático y las sombras en caché) y vuelve en su primer movimiento o pose. El registro dice `Dormant dynamic instances: N`.
- **LODs de skin**: se crean al cargar (también en una malla guardada con 1 LOD); una malla de topología difícil recibe sus LODs lejanos del simplificador de rejilla.
- **La animación lejana se actualiza menos** (como las Update Rate Optimizations de Unreal): cada cuadro hasta 15 m, luego cada 2, 4 y 8 cuadros.
- **Nanite en un Skeletal Mesh**: una malla densa (16 384 triángulos o más, o Nanite On) recibe el DAG de clusters, que sirve también a sus instancias animadas: de cerca, su lado frontal fino y su lado trasero grueso; las esferas de los clusters reciben la holgura de la pose.
- En el **visor** de Static o Skeletal Mesh, en **Nanite Settings**: **Enable Nanite Support** (Auto, On, Off: clusters en la GPU), **Fallback Triangle Percent** (lo que ven los rayos), **Collision Complexity** (Default, Simple, Complex) y **Generate Lightmap UVs**. **Apply Nanite and Collision** los guarda y el nivel los usa de inmediato. Debajo: los clusters de su DAG, el LOD de los rayos con sus triángulos y error en cm, y si es un skin rígido.

```diagram bars ms
Unamed, 5000 cápsulas: CPU antes: 31
Unamed, 5000 cápsulas: CPU ahora: 8
```
- Para comparar: `REALENGINE_NO_DORMANT=1`, `REALENGINE_NO_URO=1`.
- Consulta también: [Mallas pesadas](article:heavy-meshes), [Skinning](article:skinning).
