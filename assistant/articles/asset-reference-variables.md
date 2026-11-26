id: asset-reference-variables
kind: howto
title: Variáveis de Level, Level Sequence e outros assets | Level, Level Sequence and other asset variables | Variables de Level, Level Sequence y otros assets
keys: variavel level, variável level, level variable, variavel de nivel, variável de nível, world, soft object reference, referencia de asset, referência de asset, asset reference, level sequence variable, variavel de sequencia, variável de sequência, cinematic, cinematica, cinemática, sound variable, material variable, texture variable, static mesh variable, skeletal mesh variable, animation variable, data asset variable, particle system variable, dialogue variable, quest variable, open level variavel, open level variable, tipo level, type level, levelsequence, instance editable level
--- pt
Além do nome, um nível (e outros assets) pode ficar numa **variável tipada**, como a referência de asset da Unreal (o World/Level e as Soft Object References).

```diagram steps
My Blueprint > + Variável | Tipo: Referências de Asset > Level | Valor padrão: o seletor de assets | Get na entrada Level Name do Open Level
```
## Criar
- No painel **My Grafo**, crie uma variável e, no seletor de tipo, abra **Referências de Asset**: **Level**, **Level Sequence**, Sound, Material, Texture, Static Mesh, Skeletal Mesh, Animation Sequence, Anim Montage, Data Asset, Particle System, Dialogue, Quest, Behavior Tree, Input Action, Vehicle, Physics Asset, IK Rig, Camera Shake, PCG Graph, Physical Material, Sound Class/Mix/Attenuation/Concurrency, Submix e AI Brain. Para Widgets, use **Class Reference** da classe do Widget.
- O **valor padrão** é escolhido no **seletor de assets** já filtrado pelo tipo (miniaturas e busca).
- Marque **Instance Editable** (o olho) para cada ator do nível ter o seu valor no **Details**: dois atores da mesma classe podem abrir níveis diferentes.

## Usar
- Ligue o **Get** da variável **Level** na entrada **Level Name** de **Open Level (by Name)**, **Load Stream Level** ou **Preload Level**. O nome escrito à mão continua funcionando.
- Ligue a variável **Level Sequence** no pino **Sequence** de **Play Sequence**, **Stop Sequence** ou **Is Sequence Playing**.
- **Set**, **Instance Editable**, **SaveGame** e replicação funcionam como em qualquer variável. O valor salvo no Grafo e no nível volta ao reabrir.

## Em código
O tipo existe nas quatro linguagens: Real `var next_level: Level = "Arena"`, Python `next_level: Level = "Arena"`, C# `Level nextLevel = "Arena";`, C `Level next_level = "Arena";`. Também: `LevelSequence`, `Sound`, `Material`, `Texture`, `StaticMesh`, `SkeletalMesh`, `AnimSequence`, `DataAsset`, `ParticleSystem`, `Dialogue`, `Quest`... A variável sai igual à do Grafo (com o seletor de assets no Details).
--- en
Besides its name, a level (and other assets) can be kept in a **typed variable**, like Unreal's asset reference (the World/Level and the Soft Object References).

```diagram steps
My Blueprint > + Variable | Type: Asset References > Level | Default value: the asset picker | Get into Open Level's Level Name
```
## Create
- In the **My Graph** panel make a variable and, in the type picker, open **Asset References**: **Level**, **Level Sequence**, Sound, Material, Texture, Static Mesh, Skeletal Mesh, Animation Sequence, Anim Montage, Data Asset, Particle System, Dialogue, Quest, Behavior Tree, Input Action, Vehicle, Physics Asset, IK Rig, Camera Shake, PCG Graph, Physical Material, Sound Class/Mix/Attenuation/Concurrency, Submix and AI Brain. For Widgets, use the Widget's class **Class Reference**.
- The **default value** is picked in the **asset picker**, already filtered by the type (thumbnails and a search).
- Check **Instance Editable** (the eye) so each actor in the level has its own value in **Details**: two actors of one class can open different levels.

## Use
- Wire the **Level** variable's **Get** into the **Level Name** input of **Open Level (by Name)**, **Load Stream Level** or **Preload Level**. A typed name still works.
- Wire the **Level Sequence** variable into the **Sequence** pin of **Play Sequence**, **Stop Sequence** or **Is Sequence Playing**.
- **Set**, **Instance Editable**, **SaveGame** and replication work as for any variable. The value saved in the Graph and in the level comes back when it opens again.

## In code
The type exists in the four languages: Real `var next_level: Level = "Arena"`, Python `next_level: Level = "Arena"`, C# `Level nextLevel = "Arena";`, C `Level next_level = "Arena";`. Also: `LevelSequence`, `Sound`, `Material`, `Texture`, `StaticMesh`, `SkeletalMesh`, `AnimSequence`, `DataAsset`, `ParticleSystem`, `Dialogue`, `Quest`... The variable comes out as the Graph's (with the asset picker in Details).
--- es
Además de su nombre, un nivel (y otros assets) puede guardarse en una **variable tipada**, como la referencia a asset de Unreal (el World/Level y las Soft Object References).

```diagram steps
My Blueprint > + Variable | Tipo: Asset References > Level | Valor por defecto: el selector de assets | Get hacia el Level Name de Open Level
```
## Crear
- En el panel **My Graph** crea una variable y, en el selector de tipo, abre **Asset References**: **Level**, **Level Sequence**, Sound, Material, Texture, Static Mesh, Skeletal Mesh, Animation Sequence, Anim Montage, Data Asset, Particle System, Dialogue, Quest, Behavior Tree, Input Action, Vehicle, Physics Asset, IK Rig, Camera Shake, PCG Graph, Physical Material, Sound Class/Mix/Attenuation/Concurrency, Submix y AI Brain. Para los Widgets, usa la **Class Reference** de la clase del Widget.
- El **valor por defecto** se elige en el **selector de assets**, ya filtrado por el tipo (miniaturas y una búsqueda).
- Marca **Instance Editable** (el ojo) para que cada actor del nivel tenga su propio valor en **Detalles**: dos actores de una misma clase pueden abrir niveles distintos.

## Usar
- Conecta el **Get** de la variable **Level** a la entrada **Level Name** de **Open Level (by Name)**, **Load Stream Level** o **Preload Level**. Un nombre escrito a mano sigue funcionando.
- Conecta la variable **Level Sequence** al pin **Sequence** de **Play Sequence**, **Stop Sequence** o **Is Sequence Playing**.
- **Set**, **Instance Editable**, **SaveGame** y la replicación funcionan como en cualquier variable. El valor guardado en el Grafo y en el nivel vuelve al abrirlos de nuevo.

## En código
El tipo existe en los cuatro lenguajes: Real `var next_level: Level = "Arena"`, Python `next_level: Level = "Arena"`, C# `Level nextLevel = "Arena";`, C `Level next_level = "Arena";`. También: `LevelSequence`, `Sound`, `Material`, `Texture`, `StaticMesh`, `SkeletalMesh`, `AnimSequence`, `DataAsset`, `ParticleSystem`, `Dialogue`, `Quest`... La variable sale como la del Grafo (con el selector de assets en Detalles).
