id: trace-insights
kind: concept
title: Trace: gravar o desempenho como o Unreal Insights | Trace: recording performance like Unreal Insights | Trace: grabar el rendimiento como Unreal Insights
keys: trace, insights, unreal insights, profiler, perfil de desempenho, profiling, gravar desempenho, record performance, linha do tempo, timeline, travada, hitch, stutter, engasgo, quadro lento, slow frame, perfetto, chrome tracing, chrome://tracing, onde o tempo vai, where does the time go, threads, workers, gpu passes, timers
--- pt
```diagram steps
Ferramentas > Trace (Insights) > Iniciar Gravação
Jogue ou ande pela cena (o editor ou o Play)
Parar Gravação: o arquivo vai para <projeto>/Saved/Profiling e abre na janela
```
O **Debug Viewer** (F10) mostra o custo de agora. O **Trace** grava **cada quadro** num arquivo para você estudar depois, como o Unreal Insights: serve para achar a travada que aparece uma vez a cada 10 segundos.
- **O que entra:** os passos da thread do jogo (Grafos, física, animação, IA, multidões, áudio, partículas, plugins, gravar a cena, esperar a GPU, apresentar), o trabalho de cada **worker** (as tarefas paralelas) e os passes da **GPU** (sombras, iluminação global, passe principal, pós-processo...).
- **A janela Trace** tem três partes. No alto, um gráfico com uma barra por quadro, colorida pelo FPS, e as linhas de 200 e 60 fps; clique numa barra para ver aquele quadro. No meio, a linha do tempo, com uma trilha por thread e uma pela GPU: a roda dá zoom no mouse, arrastar move, e Shift + roda rola as trilhas. Embaixo, os **Timers**: quantas vezes, total, média e máximo de cada escopo no trecho visível.
- **O arquivo** é um trace do Chrome (.json). Também abre em **ui.perfetto.dev** ou em **chrome://tracing**.
- **No jogo empacotado** e na linha de comando: `--trace arquivo.json` grava do início ao fim.
- **Custo:** desligado, uma leitura de bool por escopo; gravando, ficou dentro do ruído da medida (cena com 2000 agentes: 269 a 276 fps com e sem).
--- en
```diagram steps
Tools > Trace (Insights) > Start Recording
Play or move around the scene (the editor or Play)
Stop Recording: the file goes to <project>/Saved/Profiling and opens in the window
```
The **Debug Viewer** (F10) shows the cost right now. **Trace** records **every frame** into a file to study later, like Unreal Insights: it is how you find the hitch that shows up once every 10 seconds.
- **What goes in:** the game thread's steps (Graphs, physics, animation, AI, crowds, audio, particles, plugins, recording the scene, waiting for the GPU, presenting), each **worker**'s work (the parallel jobs) and the **GPU**'s passes (shadows, global illumination, main pass, post process...).
- **The Trace window** has three parts. At the top, a graph with one bar per frame, colored by its FPS, and lines at 200 and 60 fps; click a bar to see that frame. In the middle, the timeline, with one track per thread and one for the GPU: the wheel zooms at the mouse, a drag pans, and Shift + wheel scrolls the tracks. At the bottom, the **Timers**: count, total, mean and max of each scope in the visible range.
- **The file** is a Chrome trace (.json). It also opens in **ui.perfetto.dev** or **chrome://tracing**.
- **In the packaged game** and on the command line: `--trace file.json` records from start to end.
- **Cost:** off, one bool read per scope; while recording, it stayed within the measurement's noise (a scene with 2000 agents: 269 to 276 fps with and without).
--- es
```diagram steps
Herramientas > Trace (Insights) > Start Recording
Juega o muévete por la escena (el editor o Jugar)
Stop Recording: el archivo va a <project>/Saved/Profiling y se abre en la ventana
```
El **Debug Viewer** (F10) muestra el costo ahora mismo. **Trace** graba **cada fotograma** en un archivo para estudiarlo después, como Unreal Insights: así se encuentra el tirón que aparece una vez cada 10 segundos.
- **Qué entra:** los pasos del hilo del juego (Grafos, física, animación, IA, multitudes, audio, partículas, plugins, grabar la escena, esperar a la GPU, presentar), el trabajo de cada **worker** (los trabajos paralelos) y los pases de la **GPU** (sombras, iluminación global, pase principal, postproceso...).
- **La ventana Trace** tiene tres partes. Arriba, un gráfico con una barra por fotograma, coloreada según sus FPS, y líneas en 200 y 60 fps; haz clic en una barra para ver ese fotograma. En el medio, la línea de tiempo, con una pista por hilo y una para la GPU: la rueda hace zoom en el ratón, arrastrar desplaza y Shift + rueda recorre las pistas. Abajo, los **Timers**: cuenta, total, media y máximo de cada ámbito en el rango visible.
- **El archivo** es un trace de Chrome (.json). También se abre en **ui.perfetto.dev** o **chrome://tracing**.
- **En el juego empaquetado** y en la línea de comandos: `--trace file.json` graba de principio a fin.
- **Costo:** desactivado, una lectura de bool por ámbito; al grabar, se mantuvo dentro del ruido de la medición (una escena con 2000 agentes: 269 a 276 fps con y sin).
