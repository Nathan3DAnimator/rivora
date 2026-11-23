id: city-routines
kind: recipe
title: Rotinas da cidade: hora do dia, lojas e testemunhas | City routines: time of day, shops and witnesses
keys: rotina da cidade, city routine, hora do dia, time of day, relogio do jogo, game clock, loja abre e fecha, shop opens and closes, horario de funcionamento, opening hours, smart object, smart objects, vagas, slots, npc vai a loja, npcs visit shops, testemunha, witness, chamar a policia, call the police, crime, report crime, cidade viva, living city, get time of day, set time of day, get police calls, is open now, get visitors, set smart object
--- pt
A cidade tem um **relógio** e lugares que os NPCs usam, como no GTA 6:
- **Relógio**: **Set Time of Day** (Hour, Time Scale: segundos de jogo por segundo real; 60 faz uma hora do jogo em um minuto) e **Get Time of Day**.
- **Smart Object**: um ator com **Interactable** e **Smart Object Slots** maior que 0 (uma loja, um quiosque, um banco). **Opens At** e **Closes At** são o horário (as duas 0: sempre aberto; 22 às 6 passa da meia-noite). **Set Smart Object** muda isso no jogo.
- **City Routine** no Crowd Spawner: os agentes vão aos Smart Objects abertos que têm vaga, ficam um tempo em volta e saem; quando fecha, todos vão embora. **Is Open Now** e **Get Visitors** dizem o estado.
- **Crime e polícia**: **Report Crime** (Location, Radius) faz a multidão ali entrar em pânico e o mais perto vira testemunha: 2 a 4 s depois ele liga para a polícia. **Get Police Calls** conta as chamadas (mande uma viatura, suba o nível de procurado).
- **Custo**: menos de 0,001 ms por agente.

```blueprint
Event Begin Play -> Set Time of Day ( Hour = 8.5, Time Scale = 60 )
Event Key C -> Report Crime ( Location = Get Actor Location, Radius = 2500 )
Event Tick -> Print String ( Get Police Calls )
```
--- en
The city has a **clock** and places the NPCs use, as in GTA 6:
- **Clock**: **Set Time of Day** (Hour, Time Scale: game seconds a real one; 60 makes a game hour in a real minute) and **Get Time of Day**.
- **Smart Object**: an actor with an **Interactable** and **Smart Object Slots** above 0 (a shop, a kiosk, a bench). **Opens At** and **Closes At** are its hours (both 0: always open; 22 to 6 wraps past midnight). **Set Smart Object** changes them in the game.
- **City Routine** on the Crowd Spawner: the agents go to the open Smart Objects with room, stay around them a while and leave; when it closes, everyone goes. **Is Open Now** and **Get Visitors** tell its state.
- **Crime and police**: **Report Crime** (Location, Radius) panics the crowd there and the nearest one becomes the witness: 2 to 4 s later they call the police. **Get Police Calls** counts the calls (send a patrol, raise the wanted level).
- **Cost**: under 0.001 ms per agent.

```blueprint
Event Begin Play -> Set Time of Day ( Hour = 8.5, Time Scale = 60 )
Event Key C -> Report Crime ( Location = Get Actor Location, Radius = 2500 )
Event Tick -> Print String ( Get Police Calls )
```
