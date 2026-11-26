id: morph-targets
kind: recipe
title: Morph targets: expressões, piscar, músculos | Morph targets: expressions, blinking, muscles | Morph targets: expresiones, parpadeo, músculos
keys: morph target, morph targets, blend shape, blend shapes, shape key, shape keys, chave de forma, expressao, expressao facial, facial expression, sorriso, sorrir, personagem sorrir, fazer sorrir, smile, make smile, rosto, cara, abrir a boca, piscar, blink, boca, mouth, visema, viseme, lip sync, musculo, muscle, corretivo, corrective, set morph target, get morph target, peso do morph, morph weight, deformar rosto, face, arkit
--- pt
Como os **Morph Targets** da Unreal: formas guardadas na malha (um sorriso, um piscar, um músculo) que você mistura por peso enquanto o jogo roda. A GPU soma `peso × deslocamento` em cada vértice antes dos ossos.
1. **Importe** um arquivo com formas: as **Shape Keys** do Tlender (modo Relativo; o grupo de vértices da chave conta) ou os **morph targets** do glTF (os nomes vêm de `extras.targetNames`, o peso padrão de `weights`). A malha vira um **Skeletal Mesh**; sem armature, um Skeletal Mesh de um osso só.
2. No ator: **Details > Skeletal Mesh > Morph Targets**, um controle por forma (0 nada, 1 a forma inteira). **Restaurar Morph Targets** volta aos pesos padrão do arquivo.
3. No Grafo (**Components > Skeletal Mesh**): **Set Morph Target** (Target, Morph Target Name, Value) e **Get Morph Target**. Sem Target ligado é o próprio ator; o nome não diferencia maiúsculas.
4. Custo: só quando um peso muda (parado não custa nada). Medido numa grade de 16 mil vértices com 2 formas, dobrada a cada quadro: ver notas-dev/animacao.md "Morph targets".

```diagram steps
Shape Keys (Tlender) ou morph targets (glTF) | Importar: Skeletal Mesh com as formas | Details: um controle por forma | Set Morph Target ( Sorriso, 1 ) | GPU: peso x deslocamento antes dos ossos
```
```blueprint
Event BeginPlay -> Set Morph Target ( Target = self, Morph Target Name = "Smile", Value = 1 )
Event Tick -> Set Morph Target ( Target = self, Morph Target Name = "Blink", Value = Get Morph Target ( Target = self, Morph Target Name = "Smile" ) )
```
```real
class Face extends Actor
    var t = 0.0

    event BeginPlay()
        SetMorphTarget("Smile", 1.0)

    event Tick(dt: float)
        t = t + dt
        if t > 3.0
            t = 0.0
        SetMorphTarget("Blink", GetMorphTarget("Smile") * t / 3.0)
```
--- en
Like Unreal's **Morph Targets**: shapes kept in the mesh (a smile, a blink, a muscle) that you mix by weight while the game runs. The GPU adds `weight × offset` to each vertex before the bones.
1. **Import** a file with shapes: the Tlender's **Shape Keys** (Relative mode; the key's vertex group counts) or glTF's **morph targets** (names from `extras.targetNames`, the default weight from `weights`). The mesh becomes a **Skeletal Mesh**; without an armature, a Skeletal Mesh of a single bone.
2. On the actor: **Details > Skeletal Mesh > Morph Targets**, a slider per shape (0 none, 1 the whole shape). **Reset Morph Targets** goes back to the file's default weights.
3. In a Graph (**Components > Skeletal Mesh**): **Set Morph Target** (Target, Morph Target Name, Value) and **Get Morph Target**. With no Target wired it is the actor itself; the name ignores case.
4. Cost: only when a weight changes (still, it costs nothing). Measured on a 16 thousand vertex grid with 2 shapes bent every frame: see notas-dev/animacao.md "Morph targets".

```diagram steps
Shape Keys (Tlender) or morph targets (glTF) | Import: a Skeletal Mesh with the shapes | Details: a slider per shape | Set Morph Target ( Smile, 1 ) | GPU: weight x offset before the bones
```
```blueprint
Event BeginPlay -> Set Morph Target ( Target = self, Morph Target Name = "Smile", Value = 1 )
Event Tick -> Set Morph Target ( Target = self, Morph Target Name = "Blink", Value = Get Morph Target ( Target = self, Morph Target Name = "Smile" ) )
```
```real
class Face extends Actor
    var t = 0.0

    event BeginPlay()
        SetMorphTarget("Smile", 1.0)

    event Tick(dt: float)
        t = t + dt
        if t > 3.0
            t = 0.0
        SetMorphTarget("Blink", GetMorphTarget("Smile") * t / 3.0)
```
--- es
Como los **Morph Targets** de Unreal: formas guardadas en la malla (una sonrisa, un parpadeo, un músculo) que mezclas por peso mientras el juego corre. La GPU suma `peso × desplazamiento` a cada vértice antes de los huesos.
1. **Importa** un archivo con formas: las **Shape Keys** de Tlender (modo Relative; cuenta el grupo de vértices de la clave) o los **morph targets** de glTF (nombres de `extras.targetNames`, el peso predeterminado de `weights`). La malla se convierte en un **Skeletal Mesh**; sin armature, un Skeletal Mesh de un solo hueso.
2. En el actor: **Detalles > Skeletal Mesh > Morph Targets**, un deslizador por forma (0 ninguna, 1 la forma entera). **Reset Morph Targets** vuelve a los pesos predeterminados del archivo.
3. En un Grafo (**Components > Skeletal Mesh**): **Set Morph Target** (Target, Morph Target Name, Value) y **Get Morph Target**. Sin Target conectado es el propio actor; el nombre ignora mayúsculas y minúsculas.
4. Costo: solo cuando un peso cambia (quieto, no cuesta nada). Medido en una malla de 16 mil vértices con 2 formas dobladas en cada cuadro: véase notas-dev/animacao.md "Morph targets".

```diagram steps
Shape Keys (Tlender) o morph targets (glTF) | Importar: un Skeletal Mesh con las formas | Detalles: un deslizador por forma | Set Morph Target ( Smile, 1 ) | GPU: peso x desplazamiento antes de los huesos
```
```blueprint
Event BeginPlay -> Set Morph Target ( Target = self, Morph Target Name = "Smile", Value = 1 )
Event Tick -> Set Morph Target ( Target = self, Morph Target Name = "Blink", Value = Get Morph Target ( Target = self, Morph Target Name = "Smile" ) )
```
```real
class Face extends Actor
    var t = 0.0

    event BeginPlay()
        SetMorphTarget("Smile", 1.0)

    event Tick(dt: float)
        t = t + dt
        if t > 3.0
            t = 0.0
        SetMorphTarget("Blink", GetMorphTarget("Smile") * t / 3.0)
```
