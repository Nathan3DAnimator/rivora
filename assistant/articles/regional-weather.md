id: regional-weather
kind: recipe
title: Clima regional: tempestades pelo mundo e poças que ficam | Regional weather: storms across the world and puddles that stay
keys: clima regional, regional weather, clima, weather, tempestade, storm, temporal, chuva em um lugar, rain in one place, sol aqui chuva ali, sunny here rain there, bairro, district, chuva localizada, localized rain, frente de rajada, gust front, nuvens da tempestade, storm clouds, poca que fica, puddle that stays, molhado persistente, persistent wetness, chao continua molhado, ground stays wet, add storm, clear storms, get rain at, get wind at, get cloudiness at, storminess
--- pt
Com o **Clima Regional**, a chuva deixa de ser uma intensidade igual no mapa todo: **tempestades** passam pelo mundo, como no GTA 6. Pode fazer sol aqui enquanto chove no bairro vizinho.
- **Ligar**: no ator **Rain**, marque **Regional Weather**. **Storminess** diz quantas tempestades passam: 0 só as que você colocar, 1 umas quatro por perto ao mesmo tempo.
- **As tempestades** nascem a barlavento, andam com o vento, crescem, ficam e se dissipam (10 a 30 min). Onde a câmera está, a chuva, o vento das gotas e as nuvens são os delas. A frente de rajada empurra o vento para fora e deixa o mar mais agitado.
- **Molhado persistente**: um mapa de 4 km em volta da câmera, em células de 16 m, guarda a água e o molhado de cada lugar. Quando você volta a um lugar, as poças da janela simulada nascem do que ficou lá. Longe da câmera, o lugar continua recebendo a chuva das tempestades e só seca pela evaporação real: uma poça dura horas.
- **Blueprint**: **Add Storm** (Location, Radius em cm, Intensity em mm/h, Duration em s) põe uma tempestade. **Clear Storms** tira todas. **Get Rain At**, **Get Wind At** e **Get Cloudiness At** dizem o clima num ponto.
- **Custo**: o mapa persistente custa ~0,02 ms de GPU por quadro (65 536 células). O campo de clima não tem estado por célula.

```diagram steps
Rain: Regional Weather | tempestades andam com o vento | chove onde elas estão | o chão guarda a água | a poça continua lá
```
```blueprint
Event Begin Play -> Add Storm ( Location = Make Vector(150000, 0, 0), Radius = 80000, Intensity = 40, Duration = 900 )
Event Tick -> Print String ( Get Rain At ( Location = Get Actor Location ) )
```
--- en
With **Regional Weather** the rain is no longer one intensity over the whole map: **storms** move across the world, as in GTA 6. It can be sunny here while it rains in the next district.
- **Turn it on**: on the **Rain** actor, check **Regional Weather**. **Storminess** says how many storms come by: 0 only the ones you put, 1 about four around at a time.
- **The storms** are born upwind, drift with the wind, grow, hold and fade (10 to 30 min). Where the camera is, the rain, the drops' wind and the clouds are theirs. Their gust front pushes the wind outward and roughens the sea.
- **Persistent wetness**: a 4 km map around the camera, in 16 m cells, keeps each place's water and wetness. When you come back to a place, the simulated window's puddles start from what was left there. Away from the camera, the place still gets the storms' rain and only dries by real evaporation: a puddle lasts hours.
- **Blueprint**: **Add Storm** (Location, Radius in cm, Intensity in mm/h, Duration in s) puts a storm. **Clear Storms** removes them all. **Get Rain At**, **Get Wind At** and **Get Cloudiness At** tell the weather at a point.
- **Cost**: the persistent map costs ~0.02 ms of GPU a frame (65 536 cells). The weather field keeps no state per cell.

```diagram steps
Rain: Regional Weather | storms drift with the wind | it rains where they are | the ground keeps the water | the puddle is still there
```
```blueprint
Event Begin Play -> Add Storm ( Location = Make Vector(150000, 0, 0), Radius = 80000, Intensity = 40, Duration = 900 )
Event Tick -> Print String ( Get Rain At ( Location = Get Actor Location ) )
```
