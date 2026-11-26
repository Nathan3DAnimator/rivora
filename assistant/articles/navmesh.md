id: navmesh
kind: recipe
title: NavMesh: onde a IA anda, Nav Links e áreas | NavMesh: where the AI walks, Nav Links and areas | NavMesh: el suelo por el que anda la IA
keys: navmesh, nav mesh, navegacao, navigation, ia pular buraco, inimigo pular vao, ia descer, nav mesh bounds volume, volume de limites, nav link, nav link proxy, pular vao, pular buraco, descer da borda, jump gap, drop down, ia nao acha caminho, ia presa, ia nao sobe escada, ai stuck, ai can't find path, mostrar navegacao, tecla p, show navigation, build paths, construir caminhos, nav modifier volume, area de custo, evitar agua, avoid water, obstaculo, ponto aleatorio alcancavel, random reachable point, project point to navigation, get path length, recast, detour
--- pt
Como na Unreal: o **NavMesh** é o chão por onde a IA anda. **AI Move To**, Behavior Tree, Real Brain, Find Random Location e o cerco dos esquadrões usam ele (sobe escadas e rampas, contorna paredes, não entra em vão estreito demais).
1. **Adicionar > Navegação > Nav Mesh Bounds Volume** e escale a caixa sobre o chão do nível. Sem nenhum volume, o nível inteiro (um nível enorme: em volta dos agentes).
2. **P** mostra o NavMesh (verde). Ele se refaz sozinho, em segundo plano: mexeu um ator Static ou Stationary, só os tiles dele são refeitos. **Compilar > Construir Caminhos** refaz tudo.
3. Um vão ou uma borda: **Nav Link Proxy**. Ponha o **Ponto Esquerdo** num lado e o **Ponto Direito** no outro (arco verde: as duas pontas no NavMesh). O Character pula o vão ou desce da borda. **Direção**: nos dois sentidos ou só um.
4. **Nav Modifier Volume**: a área dentro custa mais (**Obstáculo** 8x, **Água Rasa** 3x: a IA contorna quando pode) ou some (**Nula**).
5. O agente é o maior Character de IA do nível (raio, altura); degrau de 45 cm e rampa de até 45°.

```diagram steps
Nav Mesh Bounds Volume sobre o chão | P: o verde é onde a IA anda | Nav Link Proxy num vão: arco verde | AI Move To: a IA pula o vão
```
```blueprint
Event Tick
  -> Branch ( Get Path Length ( Get Actor Location, Get Actor Location( Get Player Character ) ) < 3000 )
       True -> AI Move To ( Pawn = self, Destination = Project Point to Navigation ( Get Actor Location( Get Player Character ) ), Acceptance Radius = 80 )
```
```real
class Wanderer extends Character
    @editable
    var radius = 1500.0
    var wait = 0.0

    event Tick(dt: float)
        wait = wait - dt
        if wait <= 0.0
            var spot = GetRandomReachablePointInRadius(GetActorLocation(), radius)
            AIMoveTo(self, spot, 60.0)
            wait = 4.0
```

Nós: **Project Point to Navigation** (o ponto do NavMesh mais perto), **Get Random Reachable Point in Radius** (um ponto aonde dá para chegar andando), **Get Path Length** (quanto se anda até lá, contornando as paredes). Veja também [IA a fundo](article:ai-pro).
--- en
As in Unreal: the **NavMesh** is the floor the AI walks on. **AI Move To**, Behavior Trees, the Real Brain, Find Random Location and the squads' surround use it (up stairs and ramps, round walls, never into a gap too narrow).
1. **Add > Navigation > Nav Mesh Bounds Volume** and scale its box over the level's floor. With no volume, the whole level (a huge one: around the agents).
2. **P** shows the NavMesh (green). It rebuilds by itself, in the background: a Static or Stationary actor moved, only its tiles are made again. **Build > Build Paths** makes everything again.
3. A gap or a ledge: **Nav Link Proxy**. Put its **Left Point** on one side and its **Right Point** on the other (green arc: both ends on the NavMesh). The Character jumps the gap or drops from the ledge. **Direction**: both ways or one.
4. **Nav Modifier Volume**: the area inside costs more (**Obstacle** 8x, **Shallow Water** 3x: the AI goes round when it can) or is gone (**Null**).
5. The agent is the level's largest AI Character (radius, height); 45 cm steps and slopes up to 45°.

```diagram steps
Nav Mesh Bounds Volume over the floor | P: the green is where the AI walks | Nav Link Proxy over a gap: green arc | AI Move To: the AI jumps the gap
```
```blueprint
Event Tick
  -> Branch ( Get Path Length ( Get Actor Location, Get Actor Location( Get Player Character ) ) < 3000 )
       True -> AI Move To ( Pawn = self, Destination = Project Point to Navigation ( Get Actor Location( Get Player Character ) ), Acceptance Radius = 80 )
```
```real
class Wanderer extends Character
    @editable
    var radius = 1500.0
    var wait = 0.0

    event Tick(dt: float)
        wait = wait - dt
        if wait <= 0.0
            var spot = GetRandomReachablePointInRadius(GetActorLocation(), radius)
            AIMoveTo(self, spot, 60.0)
            wait = 4.0
```

Nodes: **Project Point to Navigation** (the NavMesh's nearest point), **Get Random Reachable Point in Radius** (a point it can walk to), **Get Path Length** (how far the walk is, round the walls). See also [AI in depth](article:ai-pro).
--- es
Como en Unreal: el **NavMesh** es el suelo por el que anda la IA. **AI Move To**, los Behavior Trees, el Real Brain, Find Random Location y el cerco de los escuadrones lo usan (suben escaleras y rampas, rodean paredes, nunca entran en un hueco demasiado estrecho).
1. **Add > Navigation > Nav Mesh Bounds Volume** y escala su caja sobre el suelo del nivel. Sin volumen, todo el nivel (uno enorme: alrededor de los agentes).
2. **P** muestra el NavMesh (verde). Se reconstruye solo, en segundo plano: si se mueve un actor Static o Stationary, solo se rehacen sus teselas. **Build > Build Paths** lo rehace todo.
3. Un hueco o un borde: **Nav Link Proxy**. Pon su **Left Point** en un lado y su **Right Point** en el otro (arco verde: ambos extremos sobre el NavMesh). El Character salta el hueco o se deja caer del borde. **Direction**: en los dos sentidos o en uno.
4. **Nav Modifier Volume**: el área de dentro cuesta más (**Obstacle** 8x, **Shallow Water** 3x: la IA la rodea cuando puede) o desaparece (**Null**).
5. El agente es el AI Character más grande del nivel (radio, altura); escalones de 45 cm y pendientes de hasta 45°.

```diagram steps
Nav Mesh Bounds Volume sobre el suelo | P: el verde es por donde anda la IA | Nav Link Proxy sobre un hueco: arco verde | AI Move To: la IA salta el hueco
```
```blueprint
Event Tick
  -> Branch ( Get Path Length ( Get Actor Location, Get Actor Location( Get Player Character ) ) < 3000 )
       True -> AI Move To ( Pawn = self, Destination = Project Point to Navigation ( Get Actor Location( Get Player Character ) ), Acceptance Radius = 80 )
```
```real
class Wanderer extends Character
    @editable
    var radius = 1500.0
    var wait = 0.0

    event Tick(dt: float)
        wait = wait - dt
        if wait <= 0.0
            var spot = GetRandomReachablePointInRadius(GetActorLocation(), radius)
            AIMoveTo(self, spot, 60.0)
            wait = 4.0
```

Nodos: **Project Point to Navigation** (el punto más cercano del NavMesh), **Get Random Reachable Point in Radius** (un punto al que puede llegar andando), **Get Path Length** (cuánto mide el recorrido, rodeando las paredes). Consulta también [IA en profundidad](article:ai-pro).
