id: command-line
kind: concept
title: Linha de comando e testes automáticos | Command line and automated tests | Línea de comandos y pruebas automáticas
keys: ui sweep, varredura da interface, varredura de bugs, clicar em tudo, testar todos os botões, barrido de interfaz, linha de comando, argumentos, command line, cli, bench, teste automatico, ctest, realengine_background, validacao vulkan, screenshot, --shot, headless
--- pt
- `realengine.exe --project <arquivo.rproject>` abre um projeto; `--new-project <pasta> --template N` cria.
- `--bench N` mede N segundos e imprime FPS, CPU, GPU e os passes; `--shot arquivo.png` tira uma foto.
- `--validation` liga a camada de validação do Vulkan.
- `--upscaler tsr|fsr|dlss|xess` e `--upscale-mode 0-5` trocam o upscaler e o modo só nesta execução (medidas).
- `REALENGINE_BACKGROUND=1`: as janelas abrem fora da tela (testes sem atrapalhar).
- `REALENGINE_JIT=0` desliga o código nativo; `REALENGINE_ASSISTANT_URL` troca a base deste assistente.
- A suíte: `ctest --test-dir build -C Release`.
- `--ui-sweep <pasta>` (num projeto descartável: `--new-project`) clica sozinho em todos os menus, botões, janelas, menus de clique direito e no menu de pausa do Play e anota cada controle (crash, erro de validação, assert, texto sem tradução, quadro lento); `python tools/ui_sweep_report.py <pasta>` faz o relatório e a lista de bugs.
--- en
- `realengine.exe --project <file.rproject>` opens a project; `--new-project <folder> --template N` makes one.
- `--bench N` measures N seconds and prints FPS, CPU, GPU and the passes; `--shot file.png` takes a picture.
- `--validation` turns on Vulkan's validation layer.
- `--upscaler tsr|fsr|dlss|xess` and `--upscale-mode 0-5` swap the upscaler and the mode for this run only (measures).
- `REALENGINE_BACKGROUND=1`: the windows open off-screen (tests without getting in the way).
- `REALENGINE_JIT=0` turns the native code off; `REALENGINE_ASSISTANT_URL` swaps this assistant's base.
- The suite: `ctest --test-dir build -C Release`.
- `--ui-sweep <folder>` (in a throwaway project: `--new-project`) clicks by itself every menu, button, window, right click menu and Play's pause menu and notes each control (crash, validation error, assert, text without translation, slow frame); `python tools/ui_sweep_report.py <folder>` makes the report and the bug list.
--- es
- `realengine.exe --project <file.rproject>` abre un proyecto; `--new-project <folder> --template N` crea uno.
- `--bench N` mide N segundos e imprime los FPS, la CPU, la GPU y los pases; `--shot file.png` toma una foto.
- `--validation` activa la capa de validación de Vulkan.
- `--upscaler tsr|fsr|dlss|xess` y `--upscale-mode 0-5` cambian el escalador y el modo solo para esta ejecución (mediciones).
- `REALENGINE_BACKGROUND=1`: las ventanas se abren fuera de la pantalla (pruebas sin estorbar).
- `REALENGINE_JIT=0` desactiva el código nativo; `REALENGINE_ASSISTANT_URL` cambia la base de este asistente.
- La suite: `ctest --test-dir build -C Release`.
- `--ui-sweep <carpeta>` (en un proyecto desechable: `--new-project`) hace clic por sí solo en todos los menús, botones, ventanas, menús de clic derecho y en el menú de pausa de Jugar y anota cada control (crash, error de validación, assert, texto sin traducción, cuadro lento); `python tools/ui_sweep_report.py <carpeta>` genera el informe y la lista de bugs.
