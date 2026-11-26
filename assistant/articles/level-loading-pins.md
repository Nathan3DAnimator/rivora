id: level-loading-pins
kind: howto
title: Carregar níveis: Loaded, Failed e Progress | Loading levels: Loaded, Failed and Progress | Pines de carga de niveles: Loaded, Failed y Progress
keys: loaded, failed, progress, progresso, carregado, falhou, carregar nivel, carregar nível, load level, load stream level, preload level, pre-carregar, pré-carregar, open level, abrir nivel, abrir nível, tela de carregamento, loading screen, barra de progresso, progress bar, velocidade de carregamento, load speed, async loading speed, get stream level state, nivel nao existe, nível não existe, level not found, travada, hitch, streaming
--- pt
Os nós de carregamento têm três saídas novas, **embaixo** da saída de sempre:
- **Loaded**: o nível carregou.
- **Failed**: não carregou (o nível não existe no projeto, o arquivo não pôde ser lido, o pré-carregamento foi cancelado).
- **Progress** (float de 0 a 1): sobe a cada quadro enquanto carrega. Ligue numa barra de progresso (Progress Bar) ou leia num Tick.

```diagram steps
Preload Level (Load Speed 0.2) | Then segue na hora | Progress 0 → 1 | Loaded | Open Level (by Name) troca sem travar
```
## Quem abre o nível
- **Preload Level** prepara o nível inteiro na memória em segundo plano (sem BeginPlay, sem desenhar). Quem abre é o **Open Level (by Name)** desse nível: ele só troca o mundo, como um corte de câmera, sem ler o arquivo de novo. Se não terminou ainda, aparece a tela de carregamento e ele termina na velocidade máxima. Sem nada preparado, o Open Level carrega como sempre.
- **Load Stream Level** traz os atores de outro nível para dentro deste (subníveis). **Completed** dispara quando termina de qualquer jeito; depois vem **Loaded** ou **Failed**.

## Velocidade
- **Load Speed** (Preload Level e Set Level Load Speed): **mínimo 0,01** (devagar e leve, atrás de um lobby), **máximo 1** (o mais rápido); 0 usa a velocidade geral. **Set Async Loading Speed**: mínimo 0,01, máximo 1. O campo do nó não aceita valor fora disso, e o tooltip mostra os limites.

## Em código (Real, Python, C#, C)
`load_stream_level("Arena", True, False)` espera terminar; depois `get_stream_level_state("Arena")` diz o **Progress**, **Loaded** e **Failed**. `preload_level("Arena", 0.2)` segue na hora; `get_level_preload_progress("Arena")` dá o progresso, **Ready** e **Failed**.

```diagram bars ms
Open Level de um nível pré-carregado (40 000 atores): 0.029
Pior quadro enquanto pré-carregava (velocidade 0.2): 0.406
```
--- en
The loading nodes have three new outputs, **under** the usual one:
- **Loaded**: the level loaded.
- **Failed**: it didn't (no such level in the project, its file couldn't be read, the preload was cancelled).
- **Progress** (a float 0 to 1): rises every frame while it loads. Bind a Progress Bar to it or read it in a Tick.

```diagram steps
Preload Level (Load Speed 0.2) | Then goes on at once | Progress 0 → 1 | Loaded | Open Level (by Name) swaps it in without a hitch
```
## Who opens the level
- **Preload Level** prepares the whole level in memory in the background (no BeginPlay, nothing drawn). **Open Level (by Name)** of that level opens it: it only swaps the world in, like a camera cut, without reading the file again. Not finished yet: the loading screen shows and it finishes at full speed. With nothing prepared, Open Level loads as always.
- **Load Stream Level** brings another level's actors into this one (sublevels). **Completed** fires when it ends either way; then **Loaded** or **Failed**.

## Speed
- **Load Speed** (Preload Level and Set Level Load Speed): **min 0.01** (slow and light, behind a lobby), **max 1** (as fast as it goes); 0 uses the general speed. **Set Async Loading Speed**: min 0.01, max 1. The node's field takes nothing outside that, and its tooltip shows the limits.

## In code (Real, Python, C#, C)
`load_stream_level("Arena", True, False)` waits for it to end; then `get_stream_level_state("Arena")` gives **Progress**, **Loaded** and **Failed**. `preload_level("Arena", 0.2)` goes on at once; `get_level_preload_progress("Arena")` gives the progress, **Ready** and **Failed**.

```diagram bars ms
Open Level of a preloaded level (40,000 actors): 0.029
Worst frame while it preloaded (speed 0.2): 0.406
```
--- es
Los nodos de carga tienen tres salidas nuevas, **debajo** de la habitual:
- **Loaded**: el nivel se cargó.
- **Failed**: no se cargó (no existe ese nivel en el proyecto, no se pudo leer su archivo, se canceló la precarga).
- **Progress** (un float de 0 a 1): sube en cada fotograma mientras carga. Enlaza una Progress Bar a él o léelo en un Tick.

```diagram steps
Preload Level (Load Speed 0.2) | Then sigue de inmediato | Progress 0 → 1 | Loaded | Open Level (by Name) lo cambia sin tirones
```
## Quién abre el nivel
- **Preload Level** prepara todo el nivel en memoria en segundo plano (sin BeginPlay, sin dibujar nada). **Open Level (by Name)** de ese nivel lo abre: solo cambia el mundo, como un corte de cámara, sin volver a leer el archivo. Si aún no terminó: aparece la pantalla de carga y termina a toda velocidad. Sin nada preparado, Open Level carga como siempre.
- **Load Stream Level** trae los actores de otro nivel a este (subniveles). **Completed** se dispara cuando termina de cualquier modo; luego **Loaded** o **Failed**.

## Velocidad
- **Load Speed** (Preload Level y Set Level Load Speed): **mín. 0.01** (lento y ligero, detrás de un lobby), **máx. 1** (lo más rápido posible); 0 usa la velocidad general. **Set Async Loading Speed**: mín. 0.01, máx. 1. El campo del nodo no acepta nada fuera de eso, y su tooltip muestra los límites.

## En código (Real, Python, C#, C)
`load_stream_level("Arena", True, False)` espera a que termine; luego `get_stream_level_state("Arena")` da **Progress**, **Loaded** y **Failed**. `preload_level("Arena", 0.2)` sigue de inmediato; `get_level_preload_progress("Arena")` da el progreso, **Ready** y **Failed**.

```diagram bars ms
Open Level de un nivel precargado (40,000 actores): 0.029
Peor fotograma mientras precargaba (velocidad 0.2): 0.406
```
