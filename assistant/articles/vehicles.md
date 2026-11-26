id: vehicles
kind: recipe
title: Veículos: carros, motos, tanques, barcos, trens, aviões e helicópteros | Vehicles: cars, motorcycles, tanks, boats, trains, airplanes and helicopters | Vehículos: autos, motos, tanques, barcos, trenes, aviones y helicópteros
keys: veiculo, vehicle, carro, car, moto, motorcycle, caminhao, truck, tanque, tank, barco, boat, trem, train, aviao, airplane, plane, helicoptero, helicopter, dirigir, drive, pilotar, fly, chaos vehicle, wheeled vehicle, acelerador, throttle, freio, brake, marcha, gear, cambio, gearbox, entrar no carro, enter vehicle, sair do carro, exit vehicle, jogo de corrida, racing game, jogo de carro, car game
--- pt
```diagram steps
Adicionar Rápido > Veículos > Veículo (um modelo) | Detalhes: Possuir Automaticamente (ou Enter Vehicle no Blueprint) | Play: W/S acelera e freia, A/D vira, Espaço freio de mão, F portas | Duplo clique no asset: chassi, marchas e peças
```
## Como funciona
Um **Veículo** (`.rvehicle`) é um **chassi** (massa, centro de massa, caixa de colisão, arrasto) mais **peças**. Os 10 modelos já andam como estão: Carro, Carro Esportivo, Caminhão 6x6 (com caçamba basculante), Moto, Tanque (esteiras), Barco, Locomotiva, Vagão, Avião e Helicóptero.

A física é própria e profissional:
- **Suspensão por raio** com mola, amortecedor e barra estabilizadora.
- **Pneus pela fórmula mágica**, com deslizamento, ângulo de deriva e elipse de atrito; controle de tração e ABS.
- **Motor** com curva de torque, embreagem que patina na saída e limitador.
- **Câmbio** automático ou manual, e **diferencial** aberto, travado ou autoblocante.
- **Por tipo de veículo**: esteiras que giram no lugar (tanque); a moto se inclina sozinha na curva; flutuação, hélice e leme (barco); asas com estol e superfícies de controle (avião); rotor com coletivo, cíclico e estabilidade que paira ao soltar (helicóptero).

Comandos do jogador:

| Veículo | Comandos |
|---|---|
| Carro, moto, caminhão, tanque | **W/S** acelera; S freia e depois dá ré. **A/D** vira. **Espaço** freio de mão. **F/G/H/J** abrem as peças (portas, capô, caçamba). |
| Avião e helicóptero | **W/S** arfagem. **A/D** rolagem. **Q/E** guinada. **Shift/Ctrl** acelerador (avião) ou coletivo (helicóptero). |

```blueprint
Event BeginPlay -> Enter Vehicle ( Target )
```
```real
class Driver extends Actor
    event Tick(dt: float)
        SetThrottleInput(1)
        SetSteeringInput(0.3)
```
Outros nós: *Set Brake Input*, *Set Handbrake Input*, *Set Flight Input*, *Set Gear*, *Set Engine On*, *Set Part Position*, *Toggle Part*, *Exit Vehicle*, *Get Engine RPM*, *Get Current Gear*. Veja também [Peças de veículo](article:vehicle-parts) e [Água e trilhos](article:water-rails).
- **Batidas**: veículos colidem entre si; cada batida amassa (uma grade sobre a caixa), uma frente batida perde potência e uma roda dianteira batida entorta e o carro puxa para um lado. O dano fica em `vehicle_info` (Damage).
--- en
```diagram steps
Quick Add > Vehicles > Vehicle (a template) | Details: Auto Possess (or Enter Vehicle in a Blueprint) | Play: W/S throttle and brake, A/D steer, Space handbrake, F doors | Double-click the asset: chassis, gears and parts
```
## How it works
A **Vehicle** (`.rvehicle`) is a **chassis** (mass, center of mass, collision box, drag) plus **parts**. The 10 templates drive as they are: Car, Sports Car, Truck 6x6 (with a dump bed), Motorcycle, Tank (tracks), Boat, Locomotive, Train Wagon, Airplane and Helicopter.

The physics is its own, and professional:
- **Raycast suspension** with springs, dampers and anti-roll bars.
- **Tires by a magic formula**, with slip, slip angle and a friction ellipse; traction control and ABS.
- **Engine** with a torque curve, a clutch that slips pulling away, and a limiter.
- **Gearbox**, automatic or manual, and an open, locked or limited-slip **differential**.
- **Per kind**: tracks that turn on the spot (tank); a motorcycle that leans into its turns by itself; buoyancy, a propeller and a rudder (boat); wings with stall and control surfaces (airplane); a rotor with collective, cyclic and stability that hovers when let go (helicopter).

Player controls:

| Vehicle | Controls |
|---|---|
| Car, motorcycle, truck, tank | **W/S** throttle; S brakes, then backs up. **A/D** steer. **Space** handbrake. **F/G/H/J** open the parts (doors, hood, bed). |
| Airplane and helicopter | **W/S** pitch. **A/D** roll. **Q/E** yaw. **Shift/Ctrl** throttle (airplane) or collective (helicopter). |

```blueprint
Event BeginPlay -> Enter Vehicle ( Target )
```
```real
class Driver extends Actor
    event Tick(dt: float)
        SetThrottleInput(1)
        SetSteeringInput(0.3)
```
Other nodes: *Set Brake Input*, *Set Handbrake Input*, *Set Flight Input*, *Set Gear*, *Set Engine On*, *Set Part Position*, *Toggle Part*, *Exit Vehicle*, *Get Engine RPM*, *Get Current Gear*. See also [Vehicle parts](article:vehicle-parts) and [Water and rails](article:water-rails).
- **Crashes**: vehicles collide with each other; each crash dents them (a grid over the box), a crushed front loses power and a hit front wheel bends and the car pulls to one side. The damage is in `vehicle_info` (Damage).
--- es
```diagram steps
Añadir rápido > Vehículos > Vehicle (una plantilla) | Detalles: Auto Possess (o Enter Vehicle en un Blueprint) | Jugar: W/S acelerador y freno, A/D dirección, Espacio freno de mano, F puertas | Doble clic en el asset: chasis, marchas y piezas
```
## Cómo funciona
Un **Vehicle** (`.rvehicle`) es un **chasis** (masa, centro de masa, caja de colisión, resistencia) más **piezas**. Las 10 plantillas se conducen tal cual: Car, Sports Car, Truck 6x6 (con una caja volcable), Motorcycle, Tank (orugas), Boat, Locomotive, Train Wagon, Airplane y Helicopter.

La física es propia, y profesional:
- **Suspensión por raycast** con resortes, amortiguadores y barras estabilizadoras.
- **Neumáticos por una fórmula mágica**, con deslizamiento, ángulo de deslizamiento y una elipse de fricción; control de tracción y ABS.
- **Motor** con una curva de par, un embrague que patina al arrancar y un limitador.
- **Caja de cambios**, automática o manual, y un **diferencial** abierto, bloqueado o autoblocante.
- **Según el tipo**: orugas que giran en el sitio (tanque); una moto que se inclina sola en las curvas; flotabilidad, una hélice y un timón (barco); alas con pérdida de sustentación y superficies de control (avión); un rotor con colectivo, cíclico y estabilidad que se queda en el aire al soltar (helicóptero).

Controles del jugador:

| Vehículo | Controles |
|---|---|
| Auto, moto, camión, tanque | **W/S** acelerador; S frena y luego va en reversa. **A/D** dirección. **Espacio** freno de mano. **F/G/H/J** abren las piezas (puertas, capó, caja). |
| Avión y helicóptero | **W/S** cabeceo. **A/D** alabeo. **Q/E** guiñada. **Shift/Ctrl** acelerador (avión) o colectivo (helicóptero). |

```blueprint
Event BeginPlay -> Enter Vehicle ( Target )
```
```real
class Driver extends Actor
    event Tick(dt: float)
        SetThrottleInput(1)
        SetSteeringInput(0.3)
```
Otros nodos: *Set Brake Input*, *Set Handbrake Input*, *Set Flight Input*, *Set Gear*, *Set Engine On*, *Set Part Position*, *Toggle Part*, *Exit Vehicle*, *Get Engine RPM*, *Get Current Gear*. Véase también [Piezas de vehículo](article:vehicle-parts) y [Agua y rieles](article:water-rails).
- **Choques**: los vehículos chocan entre sí; cada choque los abolla (una cuadrícula sobre la caja), un frente aplastado pierde potencia y una rueda delantera golpeada se dobla y el auto se tira hacia un lado. El daño está en `vehicle_info` (Damage).
