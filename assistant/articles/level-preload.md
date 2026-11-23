id: level-preload
kind: recipe
title: Carregar o mapa no lobby sem travar (pré-carregar nível com velocidade) | Load the map in the lobby without hitches (preload a level at a speed)
keys: carregar o mapa, mapa no lobby, carregar o mapa no lobby, carregar sem travar, travar ao carregar, carregar mapa sem travar, pre carregar nivel, pré-carregar nível, preload level, carregar em segundo plano, background loading, async loading, load level async, carregar mapa no lobby, lobby, velocidade de carregamento, load speed, carregar sem travar, loading without hitches, tela de carregamento, loading screen, trocar de nivel sem travar, seamless level switch, open level instantaneo, carregar mapa enorme, big map loading, set async loading speed, get level preload progress
--- pt
```diagram steps
Lobby: Preload Level (Load Speed 0.05) | O mapa carrega aos poucos, sem BeginPlay | Todos prontos: Set Level Load Speed 1 | Open Level: entra na hora (ou tela de carregamento rápida)
```
## Como funciona
- **Preload Level** (Level Name, Load Speed) carrega um nível **por trás**, sem abri-lo: o arquivo é lido em pedaços e os atores são montados à parte. **Nada roda** (nenhum BeginPlay, nada desenhado) até a troca.
- **Load Speed** vai de **0.01** (bem devagar e leve: o lobby segue liso) a **1** (o mais rápido possível). **0** usa a velocidade geral (**Set Async Loading Speed**).
- **Set Level Load Speed** muda a velocidade **enquanto carrega**: baixa enquanto o lobby joga, alta quando todos apertaram Play.
- **Get Level Preload Progress** dá o progresso (0 a 1) e **Ready**.
- **Open Level (by Name)** de um nível pré-carregado **troca na hora, como um corte de câmera**. Se ainda não terminou, aparece a **tela de carregamento** e ele termina na velocidade máxima.
- **Set Async Loading Speed** vale para todo carregamento em segundo plano (os pré-carregamentos sem velocidade própria e o **Load Stream Level**).

```blueprint
Event BeginPlay -> Preload Level ( Level Name = "MapaGrande", Load Speed = 0.05 )
Event Custom TodosProntos -> Set Level Load Speed ( Level Name = "MapaGrande", Load Speed = 1 ) -> Open Level (by Name) ( Level Name = "MapaGrande" )
```
--- en
```diagram steps
Lobby: Preload Level (Load Speed 0.05) | The map loads little by little, no BeginPlay | Everyone ready: Set Level Load Speed 1 | Open Level: in at once (or a short loading screen)
```
## How it works
- **Preload Level** (Level Name, Load Speed) loads a level **behind**, without opening it: its file is read in slices and its actors are built apart. **Nothing runs** (no BeginPlay, nothing drawn) until the switch.
- **Load Speed** goes from **0.01** (very slow and light: the lobby stays smooth) to **1** (as fast as it goes). **0** uses the global speed (**Set Async Loading Speed**).
- **Set Level Load Speed** changes the speed **while it loads**: low while the lobby plays, high when everyone pressed Play.
- **Get Level Preload Progress** gives the progress (0 to 1) and **Ready**.
- **Open Level (by Name)** of a preloaded level **swaps it in at once, like a camera cut**. Not finished yet: the **loading screen** shows and it finishes at full speed.
- **Set Async Loading Speed** applies to every background load (the preloads without a speed of their own and **Load Stream Level**).

```blueprint
Event BeginPlay -> Preload Level ( Level Name = "BigMap", Load Speed = 0.05 )
Event Custom EveryoneReady -> Set Level Load Speed ( Level Name = "BigMap", Load Speed = 1 ) -> Open Level (by Name) ( Level Name = "BigMap" )
```
