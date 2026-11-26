id: stable-gi
kind: concept
title: GI e reflexos estáveis: sem cintilação nem rastro | Stable GI and reflections: no flicker, no trail | GI y reflejos estables: sin parpadeo ni estela
keys: estavel, estável, estabilidade temporal, temporal stability, estabilidad temporal, cintilacao, cintilação, cintilando, piscando parado, imagem tremendo, shimmer, shimmering, flicker, flickering, parpadeo, rastro atras do objeto em movimento, rastro do personagem andando, trail behind moving object, estela, ghosting behind a character, desoclusao, desoclusão, disocclusion, desoclusion, historico descoberto, uncovered history, history fix, reject uncovered history, descartar historico descoberto, denoiser history fix, correcao do historico, r.TSR.Disocclusion, r.Denoiser.HistoryFix, house_ghost, max-ghost, max-pulses, ghosting probe, sonda de ghosting
--- pt
```diagram steps
Um ator anda e descobre o fundo | TSR: a profundidade do quadro anterior diz que algo estava na frente | O histórico do que saiu é descartado | Denoiser: o pixel novo pega a luz dos vizinhos convergidos | Sem rastro, sem ferver
```
A engine acumula luz entre quadros em três lugares: o **cache de superfície** e as **sondas** (a história mora no mundo, nunca na tela), o **TSR** (anti-aliasing e upscaling temporal) e o **denoiser** do Fotorrealista. Os dois últimos guardam história por pixel, e é ali que nascem os rastros.
- **Descartar Histórico Descoberto** (Project Settings > Rendering, ligado por padrão; console `r.TSR.Disocclusion`): onde um ator em movimento descobre o fundo, o TSR compara a profundidade do quadro anterior com a deste ponto; se havia algo mais perto ali, o histórico (a cor de quem saiu) é descartado e o pixel começa da média 3x3 deste quadro. Custa ~0,03 ms e só roda enquanto algum ator se move.
- **Correção do Histórico do Denoiser** (Fotorrealista e Vídeo-realista; console `r.Denoiser.HistoryFix`): um pixel que o denoiser acabou de começar pega a luz dos vizinhos na mesma superfície que já têm muitos quadros (como o history fix do ReBLUR), em vez de um ou dois quadros de ruído. Custa ~0,06 ms.
- **Medido** (RTX 3060, 1440p, Épico, um pilar a 2 m/s passando na frente de uma porta acesa): rastro no Fotorrealista 2,6-2,8% → 1,4-1,5%; no Realista 0,75-0,81% → 0,63-0,66%. Cintilação parada e fps iguais.
- **Câmera andando:** a mudança "vai e volta" (o que pisca, não o que desliza) fica em 0,17% no Realista e 0,36% no Fotorrealista.
- **Testes:** `ghost_realistic`, `ghost_medium`, `ghost_photoreal` (`--scene house_ghost --max-ghost`), `stable_pan` e `stable_pan_photoreal` (`--cam-move ... --max-pulses`). `tools/stability_bench.ps1` mede os quatro presets.
Para comparar no jogo, desligue por Grafo:
```blueprint
Event BeginPlay
  -> Set Console Variable ( Name = "r.TSR.Disocclusion", Value = 0 )
```
--- en
```diagram steps
An actor moves and uncovers the background | TSR: last frame's depth says something stood in front | The history of what left is dropped | Denoiser: the new pixel takes its converged neighbors' light | No trail, no boiling
```
The engine accumulates light across frames in three places: the **surface cache** and the **probes** (their history lives in the world, never on the screen), the **TSR** (temporal anti-aliasing and upscaling) and the Photoreal **denoiser**. The last two keep a history per pixel, and that is where trails are born.
- **Reject Uncovered History** (Project Settings > Rendering, on by default; console `r.TSR.Disocclusion`): where a moving actor uncovers the background, the TSR compares last frame's depth with this point's; if something nearer stood there, the history (the color of what left) is dropped and the pixel starts from this frame's 3x3 mean. Costs ~0.03 ms and only runs while some actor moves.
- **Denoiser History Fix** (Photoreal and Video-realistic; console `r.Denoiser.HistoryFix`): a pixel the denoiser just started takes the light of its neighbors on the same surface that already hold many frames (like ReBLUR's history fix), instead of one or two frames of noise. Costs ~0.06 ms.
- **Measured** (RTX 3060, 1440p, Epic, a pillar at 2 m/s passing a lit doorway): the trail in Photoreal 2.6-2.8% → 1.4-1.5%; in Realistic 0.75-0.81% → 0.63-0.66%. Still flicker and fps unchanged.
- **Moving camera:** the back-and-forth change (what flickers, not what slides) stays at 0.17% in Realistic and 0.36% in Photoreal.
- **Tests:** `ghost_realistic`, `ghost_medium`, `ghost_photoreal` (`--scene house_ghost --max-ghost`), `stable_pan` and `stable_pan_photoreal` (`--cam-move ... --max-pulses`). `tools/stability_bench.ps1` measures the four presets.
To compare in the game, turn it off from a Graph:
```blueprint
Event BeginPlay
  -> Set Console Variable ( Name = "r.TSR.Disocclusion", Value = 0 )
```
--- es
```diagram steps
Un actor se mueve y descubre el fondo | TSR: la profundidad del fotograma anterior dice que algo estaba delante | Se descarta el historial de lo que se fue | Denoiser: el píxel nuevo toma la luz de sus vecinos ya convergidos | Sin estela, sin burbujeo
```
El motor acumula luz entre fotogramas en tres lugares: la **caché de superficie** y las **sondas** (su historial vive en el mundo, nunca en la pantalla), el **TSR** (antialiasing temporal y upscaling) y el **denoiser** Fotorrealista. Los dos últimos guardan un historial por píxel, y ahí nacen las estelas.
- **Reject Uncovered History** (Project Settings > Rendering, activado por defecto; consola `r.TSR.Disocclusion`): donde un actor en movimiento descubre el fondo, el TSR compara la profundidad del fotograma anterior con la de este punto; si algo más cercano estaba allí, se descarta el historial (el color de lo que se fue) y el píxel parte de la media 3x3 de este fotograma. Cuesta ~0.03 ms y solo se ejecuta mientras algún actor se mueve.
- **Denoiser History Fix** (Fotorrealista y Videorrealista; consola `r.Denoiser.HistoryFix`): un píxel que el denoiser acaba de empezar toma la luz de sus vecinos de la misma superficie que ya acumulan muchos fotogramas (como el history fix de ReBLUR), en vez de uno o dos fotogramas de ruido. Cuesta ~0.06 ms.
- **Medido** (RTX 3060, 1440p, Épico, un pilar a 2 m/s pasando por una puerta iluminada): la estela en Fotorrealista 2.6-2.8% → 1.4-1.5%; en Realista 0.75-0.81% → 0.63-0.66%. El parpadeo y los fps siguen igual.
- **Cámara en movimiento:** el cambio de ida y vuelta (lo que parpadea, no lo que se desliza) se mantiene en 0.17% en Realista y 0.36% en Fotorrealista.
- **Pruebas:** `ghost_realistic`, `ghost_medium`, `ghost_photoreal` (`--scene house_ghost --max-ghost`), `stable_pan` y `stable_pan_photoreal` (`--cam-move ... --max-pulses`). `tools/stability_bench.ps1` mide los cuatro presets.
Para comparar en el juego, desactívalo desde un Grafo:
```blueprint
Event BeginPlay
  -> Set Console Variable ( Name = "r.TSR.Disocclusion", Value = 0 )
```
