id: physics-constraint
kind: recipe
title: Juntas de física: porta, pêndulo, corrente, trilho | Physics joints: door, pendulum, chain, rail | Articulaciones de física: puerta, péndulo, cadena, riel
keys: physics constraint, restricao fisica, junta, joint, dobradica, hinge, porta que abre, door, pendulo, pendulum, corrente, chain, corda de caixas, ponte, bridge, trilho, rail, slider, prismatica, prismatic, motor, quebrar junta, break constraint, set constraint motor, fixa, fixed, ball socket, esfera
--- pt
Como o **Physics Constraint** da Unreal: segura dois atores com física um no outro (ou um no mundo).
1. Os atores precisam de **Simular Física** (Details > Adicionar > Física).
2. Num ator vazio ou qualquer outro: **Details > Adicionar > Physics Constraint**. A junta fica **onde esse ator está**; o **eixo X** dele é o eixo da dobradiça ou do trilho (gire o ator para apontar).
3. **Ator da Restrição 1** e **2** (2 vazio = preso no mundo) e a **Junta**: **Fixa** (grudados), **Dobradiça** (gira só em X; Limite de Ângulo), **Esfera** (gira para todo lado: pêndulo, corrente), **Prismática** (desliza só em X; Limite do Deslize).
4. **Motor** (dobradiça ou trilho): velocidade em graus/s ou cm/s. **Força de Quebra**: puxou mais que isso, solta. **Desligar Colisão**: os dois não batem um no outro (padrão).

```diagram steps
Porta com Simular Física | Ator com Physics Constraint na lateral, X para cima | Junta: Dobradiça, Limite 90° | Ator 1: a porta, Ator 2: vazio | Empurrar: a porta gira e para em 90°
```
```blueprint
Event BeginPlay -> Set Constraint Motor ( Target = self, Enable = true, Speed = 45 )
Event Tick -> Branch ( Is Valid ( self ) ) -> Print String ( "abrindo" )
```
```real
class Elevator extends Actor
    event BeginPlay()
        SetConstraintMotor(true, 50.0)

    event Stop()
        SetConstraintMotor(false, 0.0)

    event Explode()
        BreakConstraint()
```
--- en
Like Unreal's **Physics Constraint**: holds two actors with physics to each other (or one to the world).
1. The actors need **Simulate Physics** (Details > Add > Physics).
2. On an empty actor or any other: **Details > Add > Physics Constraint**. The joint is **where that actor is**; its **X axis** is the hinge's or the rail's axis (rotate the actor to aim it).
3. **Constraint Actor 1** and **2** (2 empty = held to the world) and the **Joint**: **Fixed** (glued), **Hinge** (turns around X only; Angle Limit), **Ball and Socket** (turns any way: pendulum, chain), **Prismatic** (slides along X only; Slide Limit).
4. **Motor** (hinge or rail): speed in degrees/s or cm/s. **Break Force**: pulled harder than that, it lets go. **Disable Collision**: the two don't hit each other (default).

```diagram steps
Door with Simulate Physics | Actor with Physics Constraint at its side, X up | Joint: Hinge, Limit 90° | Actor 1: the door, Actor 2: empty | Push: the door turns and stops at 90°
```
```blueprint
Event BeginPlay -> Set Constraint Motor ( Target = self, Enable = true, Speed = 45 )
Event Tick -> Branch ( Is Valid ( self ) ) -> Print String ( "opening" )
```
```real
class Elevator extends Actor
    event BeginPlay()
        SetConstraintMotor(true, 50.0)

    event Stop()
        SetConstraintMotor(false, 0.0)

    event Explode()
        BreakConstraint()
```
--- es
Como el **Physics Constraint** de Unreal: sujeta dos actores con física entre sí (o uno al mundo).
1. Los actores necesitan **Simulate Physics** (Detalles > Añadir > Physics).
2. En un actor vacío o en cualquier otro: **Detalles > Añadir > Physics Constraint**. La articulación está **donde está ese actor**; su **eje X** es el eje de la bisagra o del riel (gira el actor para apuntarlo).
3. **Constraint Actor 1** y **2** (2 vacío = sujeto al mundo) y el **Joint**: **Fixed** (pegado), **Hinge** (gira solo alrededor de X; Angle Limit), **Ball and Socket** (gira en cualquier dirección: péndulo, cadena), **Prismatic** (se desliza solo a lo largo de X; Slide Limit).
4. **Motor** (bisagra o riel): velocidad en grados/s o cm/s. **Break Force**: si se tira con más fuerza que eso, se suelta. **Disable Collision**: los dos no chocan entre sí (por defecto).

```diagram steps
Puerta con Simulate Physics | Actor con Physics Constraint a su lado, X hacia arriba | Joint: Hinge, Límite 90° | Actor 1: la puerta, Actor 2: vacío | Empuja: la puerta gira y se detiene a 90°
```
```blueprint
Event BeginPlay -> Set Constraint Motor ( Target = self, Enable = true, Speed = 45 )
Event Tick -> Branch ( Is Valid ( self ) ) -> Print String ( "opening" )
```
```real
class Elevator extends Actor
    event BeginPlay()
        SetConstraintMotor(true, 50.0)

    event Stop()
        SetConstraintMotor(false, 0.0)

    event Explode()
        BreakConstraint()
```
