id: splines-cables
kind: recipe
title: Splines, Spline Mesh e cabos: estrada curva, cano, cerca, fio entre postes | Splines, Spline Mesh and cables: a curved road, a pipe, a fence, a wire between poles | Splines, Spline Mesh y cables: carretera curva, tubería, valla, cable entre postes
keys: spline, splines, curva spline, spline component, seguir a spline, ao longo da spline, along spline, a lo largo de la spline, spline mesh, estrada curva, curved road, carretera, cano, pipe, tuberia, cerca, fence, valla, instancias na spline, instances along spline, tangente, tangent, roll, cabo, cable, fio, wire, alambre, corda, rope, cuerda, fio entre postes, fio pendurado entre dois postes, fio pendurado, cabo pendurado, poste, postes, pendurar, balancar, cabo de energia, linha de energia, power line, fio de luz, corrente, chain, wire between poles, pendurado, hanging, colgado, cable component, xpbd, set attach end to, get location at distance along spline, get spline length
--- pt
```diagram steps
Colocar Atores > Mundo > Spline / Spline Mesh / Spline Instances / Cable | Viewport: arraste os pontos (Alt: novo; botão direito: adicionar / excluir) | Ponto selecionado: alças ciano = tangentes | Details: Spline Mesh (malha, Eixo Frontal, Comprimento) e Instâncias (espaçamento, semente) | Cabo: Ator Inicial / Final + Socket, Comprimento, Vento
```
Uma **Spline** é o Spline Component da Unreal: pontos com tangentes, **roll** e **escala**, do tipo **Curva** (tangentes automáticas), **Curva com Tangente Própria**, **Linear** ou **Constante**, e um **Laço Fechado**. Ela é a curva única da engine: os rios e os trilhos usam a mesma avaliação.
1. **Colocar Atores > Mundo > Splines**: **Spline** (só a curva), **Spline Mesh** (uma estrada de exemplo dobrada nela), **Spline Instances** (uma cerca) ou **Cable**.
2. **Editar na viewport**: arraste um ponto (Shift: para cima e para baixo; as setas do ponto: num eixo). Alt + arrastar cria um ponto novo; o botão direito adiciona ou exclui. As **alças ciano** do ponto selecionado são as tangentes: arrastar uma vira **Curva com Tangente Própria**. Em Details > Spline: **Editar Ponto** mostra tipo, roll, escala e tangentes.
3. **Spline Mesh**: Details > Spline Mesh: a **Malha** (Estrada ou Cano de exemplo, ou uma importada), o **Eixo Frontal** (X na maioria, Z num cilindro em pé) e o **Comprimento da Malha** (0: uma por segmento). A malha é dobrada na GPU (sem cópia), com sombra, raios e movimento; o roll inclina, a escala do ponto alarga.
4. **Instâncias ao longo**: Details > Instâncias: malha, **Espaçamento**, **Semente Aleatória**, deslocamento e giro aleatórios, escala mínima e máxima, **Alinhar** (Em Pé, Ao Longo da Spline, Como o Ator) e se colidem.
5. **Cabo** (Cable Component): uma corda simulada por XPBD entre o **Ator Inicial** e o **Ator Final** (pelo nome; um **Socket** ou osso do Skeletal Mesh: uma mão, um cinto), com **Comprimento**, **Número de Segmentos**, **Rigidez**, **Rigidez à Flexão**, **Iterações**, colisão com o mundo e com corpos, **Vento**, desenhado como **Tubo** ou **Fita**. Pendura e balança no editor e no jogo.
6. **Grafo**: **Get Location at Distance Along Spline** + um Delta a cada Tick move uma plataforma, um carro ou uma câmera pela curva; **Get Transform at Distance Along Spline** dá também a rotação. **Set Attach End To** prende a ponta de um cabo a um ator ou socket; **Detach Cable End** corta.
```blueprint
Event Tick
  -> Set Distance ( Distance = Distance + Delta Seconds * 300 )
  -> Set Actor Location ( New Location = Get Location at Distance Along Spline ( Target = Track, Distance = Distance, World Space = true ) )
  -> Set Actor Rotation ( New Rotation = Get Rotation at Distance Along Spline ( Target = Track, Distance = Distance, World Space = true ) )
```
```real
class SplineRider extends Actor
    @editable
    var speed = 300.0
    var distance = 0.0
    var track: Actor

    event Tick(dt: float)
        distance = distance + dt * speed
        SetActorLocation(track.GetLocationAtDistanceAlongSpline(distance, true))
```
--- en
```diagram steps
Place Actors > World > Spline / Spline Mesh / Spline Instances / Cable | Viewport: drag the points (Alt: a new one; right button: add / delete) | The selected point: cyan handles = tangents | Details: Spline Mesh (mesh, Forward Axis, Length) and Instances (spacing, seed) | Cable: Start / End Actor + Socket, Length, Wind
```
A **Spline** is Unreal's Spline Component: points with tangents, **roll** and **scale**, of type **Curve** (automatic tangents), **Curve Custom Tangent**, **Linear** or **Constant**, and a **Closed Loop**. It is the engine's one curve: the rivers and the rails use the same evaluation.
1. **Place Actors > World > Splines**: **Spline** (the curve alone), **Spline Mesh** (an example road bent along it), **Spline Instances** (a fence) or **Cable**.
2. **Edit in the viewport**: drag a point (Shift: up and down; the point's arrows: along an axis). Alt + drag makes a new point; the right button adds or deletes. The selected point's **cyan handles** are its tangents: dragging one makes it a **Curve Custom Tangent**. In Details > Spline: **Edit Point** shows its type, roll, scale and tangents.
3. **Spline Mesh**: Details > Spline Mesh: the **Mesh** (the example Road or Pipe, or an imported one), the **Forward Axis** (X for most, Z for a standing cylinder) and the **Mesh Length** (0: one per segment). The mesh is bent on the GPU (never copied), with its shadow, rays and motion; the roll banks it, the point scale widens it.
4. **Instances along it**: Details > Instances: a mesh, **Spacing**, **Random Seed**, random offset and turn, scale min and max, **Align** (Upright, Along the Spline, As the Actor) and whether they collide.
5. **Cable** (Cable Component): a rope simulated by XPBD between the **Start Actor** and the **End Actor** (by name; a **Socket** or bone of its Skeletal Mesh: a hand, a belt), with **Length**, **Num Segments**, **Stiffness**, **Bend Stiffness**, **Iterations**, collision with the world and with bodies, **Wind**, drawn as a **Tube** or a **Ribbon**. It hangs and swings in the editor and in the game.
6. **Graph**: **Get Location at Distance Along Spline** plus a Delta each Tick moves a platform, a car or a camera along the curve; **Get Transform at Distance Along Spline** gives the rotation too. **Set Attach End To** ties a cable's end to an actor or socket; **Detach Cable End** cuts it.
```blueprint
Event Tick
  -> Set Distance ( Distance = Distance + Delta Seconds * 300 )
  -> Set Actor Location ( New Location = Get Location at Distance Along Spline ( Target = Track, Distance = Distance, World Space = true ) )
  -> Set Actor Rotation ( New Rotation = Get Rotation at Distance Along Spline ( Target = Track, Distance = Distance, World Space = true ) )
```
```real
class SplineRider extends Actor
    @editable
    var speed = 300.0
    var distance = 0.0
    var track: Actor

    event Tick(dt: float)
        distance = distance + dt * speed
        SetActorLocation(track.GetLocationAtDistanceAlongSpline(distance, true))
```
--- es
```diagram steps
Place Actors > World > Spline / Spline Mesh / Spline Instances / Cable | Ventana gráfica: arrastra los puntos (Alt: uno nuevo; botón derecho: añadir / borrar) | El punto seleccionado: manejadores cian = tangentes | Detalles: Spline Mesh (malla, Forward Axis, Length) e Instances (espaciado, semilla) | Cable: Start / End Actor + Socket, Length, Wind
```
Una **Spline** es el Spline Component de Unreal: puntos con tangentes, **roll** y **escala**, de tipo **Curve** (tangentes automáticas), **Curve Custom Tangent**, **Linear** o **Constant**, y un **Closed Loop**. Es la única curva del motor: los ríos y los rieles usan la misma evaluación.
1. **Place Actors > World > Splines**: **Spline** (la curva sola), **Spline Mesh** (una carretera de ejemplo doblada a lo largo de ella), **Spline Instances** (una valla) o **Cable**.
2. **Edita en la ventana gráfica**: arrastra un punto (Shift: arriba y abajo; las flechas del punto: a lo largo de un eje). Alt + arrastrar crea un punto nuevo; el botón derecho añade o borra. Los **manejadores cian** del punto seleccionado son sus tangentes: arrastrar uno lo convierte en **Curve Custom Tangent**. En Detalles > Spline: **Edit Point** muestra su tipo, roll, escala y tangentes.
3. **Spline Mesh**: Detalles > Spline Mesh: la **Mesh** (la Road o Pipe de ejemplo, o una importada), el **Forward Axis** (X para la mayoría, Z para un cilindro de pie) y la **Mesh Length** (0: una por segmento). La malla se dobla en la GPU (nunca se copia), con su sombra, rayos y movimiento; el roll la inclina, la escala del punto la ensancha.
4. **Instancias a lo largo**: Detalles > Instances: una malla, **Spacing**, **Random Seed**, desplazamiento y giro aleatorios, escala mínima y máxima, **Align** (Upright, Along the Spline, As the Actor) y si colisionan.
5. **Cable** (Cable Component): una cuerda simulada con XPBD entre el **Start Actor** y el **End Actor** (por nombre; un **Socket** o hueso de su Skeletal Mesh: una mano, un cinturón), con **Length**, **Num Segments**, **Stiffness**, **Bend Stiffness**, **Iterations**, colisión con el mundo y con cuerpos, **Wind**, dibujado como un **Tube** o una **Ribbon**. Cuelga y se balancea en el editor y en el juego.
6. **Grafo**: **Get Location at Distance Along Spline** más un Delta en cada Tick mueve una plataforma, un coche o una cámara a lo largo de la curva; **Get Transform at Distance Along Spline** da también la rotación. **Set Attach End To** ata el extremo de un cable a un actor o socket; **Detach Cable End** lo corta.
```blueprint
Event Tick
  -> Set Distance ( Distance = Distance + Delta Seconds * 300 )
  -> Set Actor Location ( New Location = Get Location at Distance Along Spline ( Target = Track, Distance = Distance, World Space = true ) )
  -> Set Actor Rotation ( New Rotation = Get Rotation at Distance Along Spline ( Target = Track, Distance = Distance, World Space = true ) )
```
```real
class SplineRider extends Actor
    @editable
    var speed = 300.0
    var distance = 0.0
    var track: Actor

    event Tick(dt: float)
        distance = distance + dt * speed
        SetActorLocation(track.GetLocationAtDistanceAlongSpline(distance, true))
```
