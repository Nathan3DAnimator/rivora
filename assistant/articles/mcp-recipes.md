id: mcp-recipes
kind: howto
title: IA no editor: receitas, lotes e inspeção (MCP) | AI in the editor: recipes, batches and inspection (MCP) | IA en el editor: recetas, lotes e inspección (MCP)
keys: mcp, receita, receitas, recipe, recipes, lote, lotes, batch, transacao, transação, transaction, dry run, dry_run, simular, previa, prévia, preview, inspect_level, inspecionar, inspect, o que falta, what is missing, tool_help, ajuda da ferramenta, search, busca, next, proximo passo, próximo passo, next step, ia, ai, claude, cursor, nivel de teste, nível de teste, test level, personagem jogavel, personagem jogável, playable character, material pbr, pbr material, hud, iluminacao, iluminação, lighting
--- pt
Com o **MCP** ligado (Ferramentas > Conexão com IA), a IA tem ferramentas que fazem uma tarefa inteira numa chamada só e dizem o próximo passo.

```diagram steps
get_started | inspect_level (o que falta e como corrigir) | recipe (a tarefa inteira) | screenshot | level save
```
## Receitas (`recipe`): uma tarefa, uma chamada, um Ctrl+Z
- **test_level**: chão, Player Start, céu (sol, sky light, neblina, nuvens), obstáculos em anel e a câmera enquadrando. Params: `size_m`, `obstacles`, `lighting`.
- **lighting**: um preset de céu (day, morning, sunset, cloudy, overcast, foggy, night).
- **playable_character**: uma classe Character jogável em `/Content` (terceira pessoa, primeira pessoa ou visão de cima), Default Pawn do projeto, colocada no nível.
- **pbr_material**: um material PBR a partir das texturas de uma pasta, achadas pelo nome (BaseColor, Roughness, Metallic, Normal, AO), aplicado aos atores.
- **hud**: o HUD do jogo (barras, bússola, mira, mensagens) num ator.
- `dry_run: true` responde o plano e não muda nada.

## Lotes (`batch`)
Várias chamadas numa só, como **um passo de desfazer**. Se um passo falha, tudo volta como estava (atômico); com `dry_run` roda e desfaz, para conferir antes.

## Guia
- **inspect_level**: cada problema (sem luz, sem chão, sem jogador, script com erro, erro no log, nível não salvo) com o conserto.
- **search**: tipos de ator, componentes, receitas, atores, assets, funções e artigos numa busca.
- **tool_help**: o schema e exemplos prontos de cada ferramenta.
- Os argumentos são conferidos antes de rodar: uma chave errada responde a certa ("position" → "location"). Toda resposta traz `next`, a próxima chamada provável.

```diagram bars ms
Nível de teste com as ferramentas antigas (4 chamadas): 24.0
Nível de teste com recipe test_level (1 chamada): 6.4
```
--- en
With **MCP** on (Tools > AI Connection), the AI has tools that do a whole task in one call and say the next step.

```diagram steps
get_started | inspect_level (what is missing and the fix) | recipe (the whole task) | screenshot | level save
```
## Recipes (`recipe`): one task, one call, one Ctrl+Z
- **test_level**: a ground, a Player Start, a sky (sun, sky light, fog, clouds), obstacles in a ring and the camera framing it. Params: `size_m`, `obstacles`, `lighting`.
- **lighting**: a sky preset (day, morning, sunset, cloudy, overcast, foggy, night).
- **playable_character**: a playable Character class in `/Content` (third person, first person or top-down), the project's Default Pawn, placed in the level.
- **pbr_material**: a PBR material from a folder's textures, found by their names (BaseColor, Roughness, Metallic, Normal, AO), applied to actors.
- **hud**: the game's HUD (bars, compass, crosshair, messages) on an actor.
- `dry_run: true` answers the plan and changes nothing.

## Batches (`batch`)
Several calls in one, as **one undo step**. When a step fails everything goes back as it was (atomic); with `dry_run` it runs and undoes, to check first.

## Guide
- **inspect_level**: each problem (no light, no ground, no player, a script with errors, errors in the log, an unsaved level) with its fix.
- **search**: actor types, components, recipes, actors, assets, functions and articles in one search.
- **tool_help**: each tool's schema and examples that work as they are.
- Arguments are checked before anything runs: a wrong key answers the right one ("position" → "location"). Every answer has `next`, the likely next call.

```diagram bars ms
Test level with the old tools (4 calls): 24.0
Test level with recipe test_level (1 call): 6.4
```
--- es
Con **MCP** activado (Herramientas > Conexión con IA), la IA tiene herramientas que hacen una tarea entera en una sola llamada e indican el siguiente paso.

```diagram steps
get_started | inspect_level (lo que falta y el arreglo) | recipe (la tarea completa) | screenshot | level save
```
## Recetas (`recipe`): una tarea, una llamada, un Ctrl+Z
- **test_level**: un suelo, un Player Start, un cielo (sol, sky light, niebla, nubes), obstáculos en anillo y la cámara encuadrándolo. Parámetros: `size_m`, `obstacles`, `lighting`.
- **lighting**: un preset de cielo (día, mañana, atardecer, nublado, cubierto, con niebla, noche).
- **playable_character**: una clase Character jugable en `/Content` (tercera persona, primera persona o vista cenital), el Default Pawn del proyecto, colocada en el nivel.
- **pbr_material**: un material PBR a partir de las texturas de una carpeta, encontradas por sus nombres (BaseColor, Roughness, Metallic, Normal, AO), aplicado a actores.
- **hud**: el HUD del juego (barras, brújula, mira, mensajes) en un actor.
- `dry_run: true` responde el plan y no cambia nada.

## Lotes (`batch`)
Varias llamadas en una, como **un solo paso de deshacer**. Cuando un paso falla, todo vuelve a como estaba (atómico); con `dry_run` se ejecuta y se deshace, para comprobar antes.

## Guía
- **inspect_level**: cada problema (sin luz, sin suelo, sin jugador, un script con errores, errores en el registro, un nivel sin guardar) con su arreglo.
- **search**: tipos de actor, componentes, recetas, actores, assets, funciones y artículos en una sola búsqueda.
- **tool_help**: el esquema de cada herramienta y ejemplos que funcionan tal cual.
- Los argumentos se comprueban antes de ejecutar nada: una clave equivocada responde con la correcta ("position" → "location"). Toda respuesta trae `next`, la siguiente llamada probable.

```diagram bars ms
Nivel de prueba con las herramientas antiguas (4 llamadas): 24.0
Nivel de prueba con la receta test_level (1 llamada): 6.4
```
