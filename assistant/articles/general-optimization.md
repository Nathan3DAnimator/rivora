id: general-optimization
kind: concept
title: Otimização geral: o que ficou mais leve | General optimization: what got lighter
keys: travada, travando, engasgo, engasga, hitch, stutter, travada no inicio do play, materiais compilando, compiling materials, menu de pausa trava, pause menu hitch, pause menu hitches, opcoes travam na primeira vez, options hitch first time, tesselacao pesada, tessellation cost, otimizacao geral, otimização geral, general optimization, optimizacion general, tiron, geometry tessellation, tesselacao de geometria
--- pt
```diagram steps
Medir (F10 > GPU Passes, --gpu-passes) | Achar o passe que pesa | Mudar | Medir de novo, alternando antes e depois
```
O que a otimização geral do 0.43 (Q22) mudou, sem perder qualidade:

- **Início do Play no editor**: os materiais do projeto que ainda compilam em segundo plano não fazem mais a cena inteira ser reenviada (e a GPU esperada) a cada quadro em que nada foi assado; só quando um material fica pronto.
- **Geometry Tessellation** mais leve: cada microvértice lê os cantos já prontos do seu triângulo (128 bytes que a subdivisão grava uma vez) num vertex shader só dele; o `mesh.vert` comum voltou ao tamanho de antes, e as cenas sem tesselação não pagam mais por ela. Veja [Geometry Tessellation](article:nanite-tessellation).
- **Menu de pausa e Opções**: a consulta ao DLSS, FSR e XeSS (se rodam nesta placa) é feita no início do Play, não no quadro em que as Opções aparecem (era uma travada de ~1,4 s na primeira vez).

Para medir no seu projeto: [GPU por passe](article:gpu-passes); `--shader-stats` agora mostra também os registradores do vertex shader.
--- en
```diagram steps
Measure (F10 > GPU Passes, --gpu-passes) | Find the pass that costs | Change it | Measure again, alternating before and after
```
What 0.43's general optimization (Q22) changed, with no loss of quality:

- **Play's start in the editor**: the project's materials still compiling in the background no longer send the whole scene again (and wait for the GPU) every frame nothing was baked in; only when a material is done.
- A lighter **Geometry Tessellation**: each micro vertex reads its triangle's corners already made (128 bytes the dicing writes once) in a vertex shader of its own; the common `mesh.vert` is back to its size, and scenes without tessellation no longer pay for it. See [Geometry Tessellation](article:nanite-tessellation).
- **Pause menu and Options**: DLSS, FSR and XeSS are asked whether they run on this GPU when Play starts, not in the frame the Options appear (a ~1.4 s hitch the first time).

To measure your project: [GPU per pass](article:gpu-passes); `--shader-stats` now shows the vertex shader's registers too.
--- es
```diagram steps
Medir (F10 > GPU Passes, --gpu-passes) | Encontrar el pase que pesa | Cambiarlo | Medir otra vez, alternando antes y después
```
Lo que cambió la optimización general de la 0.43 (Q22), sin perder calidad:

- **Inicio del Play en el editor**: los materiales del proyecto que aún compilan en segundo plano ya no hacen reenviar la escena entera (y esperar a la GPU) en cada cuadro en que no se horneó nada; solo cuando un material está listo.
- **Geometry Tessellation** más ligera: cada microvértice lee las esquinas ya hechas de su triángulo (128 bytes que la subdivisión escribe una vez) en un vertex shader propio; el `mesh.vert` común volvió a su tamaño y las escenas sin teselación ya no pagan por ella. Ver [Geometry Tessellation](article:nanite-tessellation).
- **Menú de pausa y Opciones**: se pregunta al DLSS, FSR y XeSS si funcionan en esta placa al empezar el Play, no en el cuadro en que aparecen las Opciones (un tirón de ~1,4 s la primera vez).

Para medir tu proyecto: [GPU por pase](article:gpu-passes); `--shader-stats` ahora muestra también los registros del vertex shader.
