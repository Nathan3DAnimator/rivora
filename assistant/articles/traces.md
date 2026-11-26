id: traces
kind: recipe
title: Traces (linha, esfera, caixa, cápsula) e desenho de debug | Traces (line, sphere, box, capsule) and debug drawing | Traces (línea, esfera, caja, cápsula) y dibujo de depuración
keys: trace complex, complexo, triangulos, async trace, assincrono, async line trace, handle, get async trace results, trace, line trace, linetrace, raycast, raio, sphere trace, box trace, capsule trace, multi trace, sweep, varredura, hit result, acertou, debug line, draw debug, desenhar debug, linha de debug, draw debug type, persistent, flush persistent debug lines, trace batch, lote de traces, muitos traces, 200 mil traces, visibilidade, line of sight, linha de visao, tiro, hitscan, chao embaixo, trace channel, canal
--- pt
Os nós ficam em **Collision > Traces** e **Collision > Debug**, como na Unreal.

```diagram steps
Start e End | Trace (linha, esfera, caixa ou cápsula) | Return Value e Location, Normal, Hit Actor, Distance | Draw Debug Type mostra na tela
```

- **Line / Sphere / Box / Capsule Trace By Channel**: a primeira coisa no caminho. **Multi ...** devolve todos os atores
  atravessados, do mais perto ao mais longe.
- **Trace Channel**: Visibility e Camera veem tudo que colide; World Static, World Dynamic, Pawn ou Landscape só aquele
  tipo. O ator que roda o nó fica de fora sozinho (o Ignore Self da Unreal), e os de **Actors to Ignore** também.
- **Draw Debug Type**: None, For One Frame, For Duration (Draw Time segundos) ou Persistent (até **Flush Persistent
  Debug Lines**). Vermelho até o acerto, verde depois, um ponto no impacto; nas formas, a forma no ponto onde parou.
  **Set Debug Trace Colors** troca as cores.
- **Line Trace Batch** e **Sphere Trace Batch**: arrays de Starts e Ends, rodados em paralelo em todos os núcleos.
  Use quando forem milhares por quadro (centenas de milhares cabem). Com For One Frame, os próprios núcleos escrevem o
  desenho, e a GPU desenha linhas de 1 pixel.
- **Draw Debug Line / Point / Box / Sphere / Capsule**: Duration 0 é só este quadro; abaixo de 0 fica até o Flush.
- O custo aparece no **Debug Viewer (F10) > Scene Queries**.

```blueprint
Event Tick
  -> Line Trace By Channel ( Start = Get Actor Location, End = Get Actor Location + Make Vector( 0, 0, -500 ), Draw Debug Type = For One Frame )
  -> Branch ( Condition = Return Value )
  -> Print String ( In String = Distance )
```
```real
class GroundProbe extends Actor
    var ground = 0.0

    event Tick(dt: float)
        var here = GetActorLocation()
        DrawDebugLine(here, here + Vector(0, 0, -500), Color(1, 0, 0, 1), 0.0, 1.0)
        DrawDebugSphere(here, 25.0, Color(0, 1, 0, 1), 0.0, 1.0)
```

Os traces batem nas caixas de colisão dos atores, nas cápsulas dos Characters e no Landscape (exato, triângulo a
triângulo). Com **Trace Complex** ligado, batem nos triângulos da malha do ator (a linha exata; esfera, caixa e
cápsula também). **Async Line Trace Batch** e **Async Sphere Trace Batch** rodam em segundo plano sem esperar: guarde
o Handle e leia com **Get Async Trace Results** no quadro seguinte (Ready verdadeiro).
--- en
The nodes are under **Collision > Traces** and **Collision > Debug**, as in Unreal.

```diagram steps
Start and End | Trace (line, sphere, box or capsule) | Return Value and Location, Normal, Hit Actor, Distance | Draw Debug Type shows it
```

- **Line / Sphere / Box / Capsule Trace By Channel**: the first thing in the way. **Multi ...** returns every actor it
  goes through, nearest first.
- **Trace Channel**: Visibility and Camera see everything that collides; World Static, World Dynamic, Pawn or Landscape
  only that kind. The actor running the node is left out by itself (Unreal's Ignore Self), and so are the **Actors to
  Ignore**.
- **Draw Debug Type**: None, For One Frame, For Duration (Draw Time seconds) or Persistent (until **Flush Persistent
  Debug Lines**). Red to the hit, green after it, a point on the impact; the shapes also show where they stopped.
  **Set Debug Trace Colors** changes the colors.
- **Line Trace Batch** and **Sphere Trace Batch**: arrays of Starts and Ends, run in parallel on every core. Use them
  for thousands a frame (hundreds of thousands fit). With For One Frame the cores write the drawing themselves and the
  GPU draws 1-pixel lines.
- **Draw Debug Line / Point / Box / Sphere / Capsule**: Duration 0 is this frame only; below 0 stays until the Flush.
- The cost shows in the **Debug Viewer (F10) > Scene Queries**.

```blueprint
Event Tick
  -> Line Trace By Channel ( Start = Get Actor Location, End = Get Actor Location + Make Vector( 0, 0, -500 ), Draw Debug Type = For One Frame )
  -> Branch ( Condition = Return Value )
  -> Print String ( In String = Distance )
```
```real
class GroundProbe extends Actor
    var ground = 0.0

    event Tick(dt: float)
        var here = GetActorLocation()
        DrawDebugLine(here, here + Vector(0, 0, -500), Color(1, 0, 0, 1), 0.0, 1.0)
        DrawDebugSphere(here, 25.0, Color(0, 1, 0, 1), 0.0, 1.0)
```

The traces hit the actors' collision boxes, the Characters' capsules and the Landscape (exactly, triangle by triangle).
With **Trace Complex** on they hit the actor's mesh triangles (the line exactly; spheres, boxes and capsules too).
**Async Line Trace Batch** and **Async Sphere Trace Batch** run in the background without waiting: keep the Handle and
read it with **Get Async Trace Results** the next frame (Ready true).
--- es
Los nodos están en **Collision > Traces** y **Collision > Debug**, como en Unreal.

```diagram steps
Start y End | Trace (línea, esfera, caja o cápsula) | Return Value y Location, Normal, Hit Actor, Distance | Draw Debug Type lo muestra
```

- **Line / Sphere / Box / Capsule Trace By Channel**: lo primero que se interpone. **Multi ...** devuelve todos los actores
  que atraviesa, el más cercano primero.
- **Trace Channel**: Visibility y Camera ven todo lo que colisiona; World Static, World Dynamic, Pawn o Landscape
  solo ese tipo. El actor que ejecuta el nodo queda fuera por sí solo (el Ignore Self de Unreal), igual que los **Actors to
  Ignore**.
- **Draw Debug Type**: None, For One Frame, For Duration (Draw Time segundos) o Persistent (hasta **Flush Persistent
  Debug Lines**). Rojo hasta el impacto, verde después, un punto en el impacto; las formas también muestran dónde se detuvieron.
  **Set Debug Trace Colors** cambia los colores.
- **Line Trace Batch** y **Sphere Trace Batch**: arrays de Starts y Ends, ejecutados en paralelo en todos los núcleos. Úsalos
  para miles por fotograma (caben cientos de miles). Con For One Frame los núcleos escriben el dibujo ellos mismos y la
  GPU dibuja líneas de 1 píxel.
- **Draw Debug Line / Point / Box / Sphere / Capsule**: Duration 0 es solo este fotograma; menor que 0 se queda hasta el Flush.
- El costo aparece en **Debug Viewer (F10) > Scene Queries**.

```blueprint
Event Tick
  -> Line Trace By Channel ( Start = Get Actor Location, End = Get Actor Location + Make Vector( 0, 0, -500 ), Draw Debug Type = For One Frame )
  -> Branch ( Condition = Return Value )
  -> Print String ( In String = Distance )
```
```real
class GroundProbe extends Actor
    var ground = 0.0

    event Tick(dt: float)
        var here = GetActorLocation()
        DrawDebugLine(here, here + Vector(0, 0, -500), Color(1, 0, 0, 1), 0.0, 1.0)
        DrawDebugSphere(here, 25.0, Color(0, 1, 0, 1), 0.0, 1.0)
```

Los traces golpean las cajas de colisión de los actores, las cápsulas de los Characters y el Landscape (con exactitud, triángulo a triángulo).
Con **Trace Complex** activado golpean los triángulos de la malla del actor (la línea con exactitud; las esferas, cajas y cápsulas también).
**Async Line Trace Batch** y **Async Sphere Trace Batch** se ejecutan en segundo plano sin esperar: guarda el Handle y
léelo con **Get Async Trace Results** en el fotograma siguiente (Ready true).
