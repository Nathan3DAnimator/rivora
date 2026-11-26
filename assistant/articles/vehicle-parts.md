id: vehicle-parts
kind: concept
title: Peças de veículo: rodas, suspensão, motor, pistões, portas | Vehicle parts: wheels, suspension, engine, pistons, doors | Piezas de vehículo: ruedas, suspensión, motor, pistones, puertas
keys: peca de veiculo, vehicle part, roda, wheel, suspensao, suspension, mola, spring, amortecedor, damper, shock, pistao, piston, hidraulico, hydraulic, engrenagem, gear, porta, door, capo, hood, cacamba, dump bed, dobradica, hinge, corredica, slider, helice, propeller, asa, wing, aileron, profundor, elevator, leme, rudder, rotor, boia, pontoon, float, truque, bogie, diferencial, differential, cambio, gearbox, motor, engine, torque
--- pt
```diagram steps
Duplo clique no Veículo | + Peça (o tipo) | Componente: a malha do ator que ela move (pelo nome) | Posição e eixo na vista de cima e de lado | Os números da peça à direita
```
## As peças
Cada peça é física e pode mover um **componente** do ator pelo nome (a malha da roda, o painel da porta).

**Rodas e suspensão**
- **Roda**: raio, mola (N/cm), amortecedor, curso da suspensão, esterço máximo, a parte que recebe do motor (Tração), freio, freio de mão e aderência. A malha gira, esterça e sobe e desce na mola.
- **Pistão**: estica entre o seu ponto e a peça ligada (o amortecedor até o cubo da roda, o macaco da caçamba). Sem ligação, sai pelo eixo conforme a sua entrada.

**Motor e transmissão**
- **Motor**: torque máximo, RPM de pico, máximo e de marcha lenta, e freio motor.
- **Câmbio**: relação final, automático (troca pelas RPM) ou manual. As marchas ficam no chassi.
- **Diferencial**: aberto, travado ou autoblocante.

**Peças móveis**
- **Dobradiça**: portas, capô, caçamba ou canhão giram entre Mín. e Máx. Abrem pela entrada (F/G/H/J) ou pelos nós *Set Part Position* e *Toggle Part*.
- **Corrediça**: porta de correr, braço de guindaste, garfo de empilhadeira.
- **Engrenagem**: gira com outra peça: `Engine` (virabrequim, ventoinha), `Steer` (volante, guidão), uma roda ou outra peça.

**Água e ar**
- **Hélice**: empuxo pelo acelerador; no ar (avião) ou na água (barco).
- **Asa**: sustentação e arrasto pelo ângulo, com estol. Com uma entrada vira **aileron**, **profundor**, **leme** ou **leme de barco** (Na Água).
- **Rotor**: helicóptero, com coletivo, cíclico, torque de cauda e Estabilidade.
- **Boia (Pontoon)**: flutuação e arrasto da água nos pontos do casco.

**Trilhos**
- **Truque (Bogie)**: o truque de um trem, sobre o trilho.

Centro de massa baixo deixa o veículo estável; mola mais dura e amortecedor maior deixam a suspensão firme.
--- en
```diagram steps
Double-click the Vehicle | + Part (its kind) | Component: the actor's mesh it moves (by name) | Its position and axis in the top and side views | Its numbers on the right
```
## The parts
Each part is physics and may move an actor's **component** by name (a wheel's mesh, a door's panel).

**Wheels and suspension**
- **Wheel**: radius, spring (N/cm), damper, suspension travel, max steer, its share of the engine (Drive), brake, handbrake and grip. Its mesh spins, steers and rides its spring.
- **Piston**: stretches between its point and the linked part (a shock to a wheel's hub, a dump bed's ram). Without a link it extends along its axis by its input.

**Engine and drivetrain**
- **Engine**: max torque, peak, max and idle RPM, and engine braking.
- **Gearbox**: final drive, automatic (shifts by RPM) or manual. The gear ratios live on the chassis.
- **Differential**: open, locked or limited slip.

**Moving parts**
- **Hinge**: doors, hood, bed or gun swing between Min and Max. They open by their input (F/G/H/J) or by the *Set Part Position* and *Toggle Part* nodes.
- **Slider**: a sliding door, a crane's arm, a forklift's mast.
- **Gear**: turns with another part: `Engine` (crankshaft, fan), `Steer` (steering wheel, handlebar), a wheel or another part.

**Water and air**
- **Propeller**: thrust by the throttle, in the air (airplane) or in the water (boat).
- **Wing**: lift and drag by its angle, stalling. With an input it becomes an **aileron**, **elevator**, **rudder** or a **boat's rudder** (In Water).
- **Rotor**: a helicopter's, with collective, cyclic, tail torque and Stability.
- **Pontoon**: buoyancy and water drag at the hull's points.

**Rails**
- **Bogie**: a train's truck, on the rail.

A low center of mass keeps the vehicle steady; a stiffer spring and a bigger damper make the suspension firm.
--- es
```diagram steps
Doble clic en el Vehicle | + Part (su tipo) | Component: la malla del actor que mueve (por nombre) | Su posición y eje en las vistas superior y lateral | Sus números a la derecha
```
## Las piezas
Cada pieza es física y puede mover un **componente** del actor por nombre (la malla de una rueda, el panel de una puerta).

**Ruedas y suspensión**
- **Wheel**: radio, resorte (N/cm), amortiguador, recorrido de la suspensión, giro máximo, su parte del motor (Drive), freno, freno de mano y agarre. Su malla gira, dirige y sube y baja sobre su resorte.
- **Piston**: se estira entre su punto y la pieza enlazada (un amortiguador a la mangueta de una rueda, el cilindro de una caja volquete). Sin enlace se extiende a lo largo de su eje según su entrada.

**Motor y transmisión**
- **Engine**: par máximo, pico, RPM máximas y de ralentí, y freno motor.
- **Gearbox**: reducción final, automática (cambia por RPM) o manual. Las relaciones de marchas están en el chasis.
- **Differential**: abierto, bloqueado o de deslizamiento limitado.

**Piezas móviles**
- **Hinge**: puertas, capó, caja o cañón oscilan entre Min y Max. Se abren con su entrada (F/G/H/J) o con los nodos *Set Part Position* y *Toggle Part*.
- **Slider**: una puerta corrediza, el brazo de una grúa, el mástil de un montacargas.
- **Gear**: gira con otra pieza: `Engine` (cigüeñal, ventilador), `Steer` (volante, manillar), una rueda u otra pieza.

**Agua y aire**
- **Propeller**: empuje según el acelerador, en el aire (avión) o en el agua (barco).
- **Wing**: sustentación y resistencia según su ángulo, con pérdida de sustentación. Con una entrada pasa a ser un **alerón**, **elevador**, **timón** o **timón de barco** (In Water).
- **Rotor**: el de un helicóptero, con colectivo, cíclico, par de cola y Stability.
- **Pontoon**: flotabilidad y resistencia del agua en los puntos del casco.

**Rieles**
- **Bogie**: el bogie de un tren, sobre el riel.

Un centro de masa bajo mantiene estable el vehículo; un resorte más rígido y un amortiguador mayor hacen la suspensión firme.
