id: crowds
kind: howto
title: Multidões | Crowds | Multitudes
keys: multidao, horda, exercito, muitos personagens, crowd, zumbis, pessoas, npcs, pedestres, pedestrians, cidade viva, living city, panico, panic, tiro na multidao, shot into a crowd, alarme, alarm, crowd alarm
--- pt
- **Ator > Adicionar > Crowd Spawner**: milhares de agentes com campo de fluxo, cercos e LOD com orçamento. [Adicionar](add:Crowd)
- No Details escolha a quantidade, o alvo e o modo (Hive ou Individual). Para **pedestres de uma cidade**, Comportamento **Wander** e Visão 0.
- **Variedade**: **Looks** (0 a 1) dá a cada agente seu tom de pele, as cores da roupa e o corpo, pela semente: 200 agentes, 200 pessoas diferentes, sem custo a mais.
- **Pânico**: o nó **Crowd Alarm** (Location, Radius) é um tiro ou uma explosão. Quem está no raio ouve como uma onda (15 m/s) e foge por 6 a 12 s. No máximo 64 entram em pânico por quadro, os mais perto primeiro: um tiro numa multidão de milhares nunca derruba o FPS de uma vez.
- Longe, a multidão fica mais barata sozinha: quem está a mais de 30 m pensa em quadros alternados, e a mais de 60 m só as 2 maiores partes do corpo são desenhadas.
```blueprint
Event Key 1 -> Crowd Alarm ( Location = Get Actor Location, Radius = 3500 )
```
--- en
- **Actor > Add > Crowd Spawner**: thousands of agents with a flow field, encircling and a budgeted LOD. [Add one](add:Crowd)
- In Details pick the count, the target and the mode (Hive or Individual). For a **city's pedestrians**, Behavior **Wander** and Sight 0.
- **Variety**: **Looks** (0 to 1) gives each agent its own skin tone, clothes colors and build, by the seed: 200 agents, 200 different people, at no extra cost.
- **Panic**: the **Crowd Alarm** node (Location, Radius) is a shot or an explosion. Those within the radius hear it as a wave (15 m/s) and run from it for 6 to 12 s. At most 64 start to panic a frame, the nearest first: a shot into a crowd of thousands never drops the FPS at once.
- Far away the crowd gets cheaper by itself: past 30 m they think every other frame, past 60 m only the body's 2 biggest parts are drawn.
```blueprint
Event Key 1 -> Crowd Alarm ( Location = Get Actor Location, Radius = 3500 )
```
--- es
- **Actor > Añadir > Crowd Spawner**: miles de agentes con un campo de flujo, cerco y un LOD con presupuesto. [Añadir uno](add:Crowd)
- En Detalles elige la cantidad, el objetivo y el modo (Hive o Individual). Para los **peatones de una ciudad**, Behavior **Wander** y Sight 0.
- **Variedad**: **Looks** (0 a 1) da a cada agente su propio tono de piel, colores de ropa y complexión, según la semilla: 200 agentes, 200 personas distintas, sin costo extra.
- **Pánico**: el nodo **Crowd Alarm** (Location, Radius) es un disparo o una explosión. Quienes están dentro del radio lo oyen como una onda (15 m/s) y huyen de él durante 6 a 12 s. Como máximo 64 entran en pánico por cuadro, los más cercanos primero: un disparo en una multitud de miles nunca hunde los FPS de golpe.
- A lo lejos la multitud se abarata sola: pasados 30 m piensan un cuadro sí y otro no, pasados 60 m solo se dibujan las 2 partes más grandes del cuerpo.
```blueprint
Event Key 1 -> Crowd Alarm ( Location = Get Actor Location, Radius = 3500 )
```
