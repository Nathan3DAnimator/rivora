id: test-suite
kind: concept
title: Suíte de testes da engine (para profissionais) | The engine's test suite (for professionals) | La suite de pruebas de la engine (para profesionales)
keys: testes, suite, suíte, ctest, regressao, regression, areas, etiquetas, labels, tools/suite.ps1, testar a engine, run tests, antes de publicar, before publishing, referencias, references
--- pt
```diagram steps
Confere a máquina (GPU livre) | Sem janela: em paralelo | Com janela: um por vez | Resumo: cada área PASSOU ou FALHOU
```
- Para quem compila a Rivora Engine a partir do código: `tools/suite.ps1 -Visivel` roda **a suíte**, 142 testes em 22 áreas (abrir e criar projetos, editor, importar, render, materiais, animação, física, grafos e scripts, IA, áudio, UI do jogo, níveis, multiplayer, empacotar para PC e Android, atualizador, MCP, plugins, Assistente, memória e travadas...), em ~40 min. É a que vale antes de publicar.
- No fim, uma tela com cada área PASSOU/FALHOU, a linha de erro de cada falha, o tempo e o log. `-Area render,fisica` roda só essas áreas; `-Falhas` repete só o que falhou; `-Listar` mostra as áreas.
- Antes de começar ela confere se a GPU está livre: um programa que a divide (o OVRServer do Quest, um ffmpeg gravando, o editor instalado aberto) faz os testes de fps falharem por tempo, não por erro. Ela diz o que está aberto e para; `-Forcar` segue mesmo assim.
- O render é comparado a fotos de referência (a casa em Épico, Alto, Baixo e com ray tracing): quando difere, grava ao lado uma imagem da diferença.
- Os outros testes registrados no `ctest` são os de regressão, um por bug corrigido; rodam pelo nome (`ctest -R nome`).
- Veja também: [Orçamento do quadro](article:performance-budget), [Debug Viewer](article:debug-viewer).
--- en
```diagram steps
Checks the machine (a free GPU) | No window: side by side | With a window: one at a time | Summary: each area PASSED or FAILED
```
- For those who build the Rivora Engine from source: `tools/suite.ps1 -Visivel` runs **the suite**, 142 tests in 22 areas (opening and creating projects, the editor, importing, rendering, materials, animation, physics, graphs and scripts, AI, audio, game UI, levels, multiplayer, packaging for PC and Android, the updater, MCP, plugins, the Assistant, memory and hitches...), in ~40 min. It is the one that counts before publishing.
- At the end, one screen with each area PASSED/FAILED, each failure's error line, the time and the log. `-Area render,fisica` runs only those areas; `-Falhas` repeats only what failed; `-Listar` shows the areas.
- Before starting it checks that the GPU is free: a program sharing it (the Quest's OVRServer, an ffmpeg recording, the installed editor) makes the fps tests fail on time, not on a bug. It says what is open and stops; `-Forcar` goes on anyway.
- The rendering is compared to reference pictures (the house at Epic, High, Low and with ray tracing): when it differs, a picture of the difference is written beside it.
- The other tests registered in `ctest` are the regression tests, one per fixed bug; they run by name (`ctest -R name`).
- See also: [Frame budget](article:performance-budget), [Debug Viewer](article:debug-viewer).
--- es
```diagram steps
Revisa la máquina (GPU libre) | Sin ventana: en paralelo | Con ventana: una a la vez | Resumen: cada área PASÓ o FALLÓ
```
- Para quien compila la Rivora Engine desde el código fuente: `tools/suite.ps1 -Visivel` ejecuta **la suite**, 142 pruebas en 22 áreas (abrir y crear proyectos, el editor, importar, render, materiales, animación, física, grafos y scripts, IA, audio, UI del juego, niveles, multijugador, empaquetar para PC y Android, el actualizador, MCP, plugins, el Asistente, memoria y tirones...), en ~40 min. Es la que vale antes de publicar.
- Al final, una pantalla con cada área PASÓ/FALLÓ, la línea de error de cada fallo, el tiempo y el log. `-Area render,fisica` ejecuta solo esas áreas; `-Falhas` repite solo lo que falló; `-Listar` muestra las áreas.
- Antes de empezar revisa que la GPU esté libre: un programa que la comparte (el OVRServer del Quest, un ffmpeg grabando, el editor instalado) hace que las pruebas de fps fallen por tiempo, no por un error. Dice qué está abierto y se detiene; `-Forcar` sigue de todos modos.
- El render se compara con fotos de referencia (la casa en Épico, Alto, Bajo y con ray tracing): cuando difiere, guarda al lado una imagen de la diferencia.
- Las demás pruebas registradas en `ctest` son las de regresión, una por error corregido; se ejecutan por nombre (`ctest -R nombre`).
- Consulta también: [Presupuesto del cuadro](article:performance-budget), [Debug Viewer](article:debug-viewer).
