id: coin-pickup
kind: recipe
title: Moeda que gira e é coletada | A spinning coin to pick up | Una moneda que gira y se recoge
keys: moeda, coletar, pegar item, coletavel, coin, pickup, girar objeto, overlap, sobreposicao, destruir ator, item que some
--- pt
Gira sem parar e some quando algo encosta (precisa de colisão de sobreposição: um Trigger ou uma forma com colisão).

```blueprint
Event Tick -> Add Actor Local Rotation ( Make Rotator( 0, Spin Speed * Delta Seconds, 0 ) )
Event ActorBeginOverlap -> Print String ( "Moeda!" ) -> Destroy Actor
```
```real
class Coin extends Actor
    @editable
    var spin_speed = 120.0

    event Tick(dt: float)
        AddActorLocalRotation(Rotator(0, spin_speed * dt, 0))

    event OnBeginOverlap(other: Actor)
        print("Coin!")
        DestroyActor()
```

Para contar as moedas, veja [Contar pontos](article:score-counter).
--- en
It spins forever and goes when something touches it (it needs overlap collision: a Trigger or a shape with collision).

```blueprint
Event Tick -> Add Actor Local Rotation ( Make Rotator( 0, Spin Speed * Delta Seconds, 0 ) )
Event ActorBeginOverlap -> Print String ( "Coin!" ) -> Destroy Actor
```
```real
class Coin extends Actor
    @editable
    var spin_speed = 120.0

    event Tick(dt: float)
        AddActorLocalRotation(Rotator(0, spin_speed * dt, 0))

    event OnBeginOverlap(other: Actor)
        print("Coin!")
        DestroyActor()
```

To count the coins, see [Count points](article:score-counter).
--- es
Gira para siempre y desaparece cuando algo la toca (necesita colisión de superposición: un Trigger o una forma con colisión).

```blueprint
Event Tick -> Add Actor Local Rotation ( Make Rotator( 0, Spin Speed * Delta Seconds, 0 ) )
Event ActorBeginOverlap -> Print String ( "Coin!" ) -> Destroy Actor
```
```real
class Coin extends Actor
    @editable
    var spin_speed = 120.0

    event Tick(dt: float)
        AddActorLocalRotation(Rotator(0, spin_speed * dt, 0))

    event OnBeginOverlap(other: Actor)
        print("Coin!")
        DestroyActor()
```

Para contar las monedas, mira [Contar puntos](article:score-counter).
