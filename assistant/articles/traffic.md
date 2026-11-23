id: traffic
kind: howto
title: Trânsito: carros nas ruas sozinhos | Traffic: cars driving the streets by themselves
keys: transito, traffic, carros, cars, ruas, streets, via, road, semaforo, traffic light, signal, cruzamento, junction, intersection, fila, queue, cidade, city, npc cars
--- pt
```diagram steps
Spline com Traffic Road | Pontas a menos de 6 m: cruzamento | Signal Green Time: semáforo | Jogar: carros andam, param no vermelho, viram
```
- Coloque uma **Spline** (Ator > Adicionar > Spline) e, no Details > **Road**, marque **Traffic Road**. Quando o jogo roda, carros andam nas faixas dela sozinhos.
- **Lanes Each Way** (1-4), **One Way**, **Speed Limit** (km/h), **Traffic Density** (carros a cada 100 m de faixa), **Lane Width**.
- **Cruzamentos**: a ponta de uma via a menos de 6 m do começo de outra. Onde três vias ou mais se encontram, **Signal Green Time** liga o semáforo (o verde de cada direção, 2 s de tudo vermelho entre elas).
- Cada carro é um motorista com velocidade, aceleração, frenagem e distância próprias: mantém a distância do carro da frente, faz fila no vermelho, espera o começo da próxima faixa liberar e escolhe uma saída no cruzamento.
- Custo: 1568 carros numa grade de 112 vias, 0,5 ms de CPU.
--- en
```diagram steps
Spline with Traffic Road | Ends within 6 m: a junction | Signal Green Time: lights | Play: cars drive, stop at red, turn
```
- Place a **Spline** (Actor > Add > Spline) and in Details > **Road** check **Traffic Road**. When the game plays, cars drive its lanes by themselves.
- **Lanes Each Way** (1-4), **One Way**, **Speed Limit** (km/h), **Traffic Density** (cars per 100 m of lane), **Lane Width**.
- **Junctions**: a road's end within 6 m of another's start. Where three roads or more meet, **Signal Green Time** turns the lights on (each direction's green, 2 s of all red between).
- Each car is a driver with its own speed, acceleration, braking and distance: it keeps its distance from the car ahead, queues at the red, waits for the next lane's start to clear and picks a way out at the junction.
- Cost: 1568 cars on a grid of 112 roads, 0.5 ms of CPU.
