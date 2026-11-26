id: plugins
kind: recipe
title: Plugins: nós e ferramentas em código nativo | Plugins: nodes and tools in native code | Plugins (extender el motor con una DLL)
keys: gameplay framework, desligar modulos, desativar inventario, desativar interacao, plugin embutido, built-in, plugin, plugins, criar plugin, make a plugin, new plugin, novo plugin, dll, extensao, extension, modulo nativo, native module, codigo nativo, native code, c plugin, blueprint function library, biblioteca de funcoes, nos novos, new nodes, edit plugins, ativar plugin, enable plugin, desativar plugin, disable plugin, plugin.json, sdk, grafo, grafos, graph, graphs
--- pt
```diagram steps
Editar > Plugins > Novo Plugin...: escreva o nome (ex.: MeuPlugin; marque C++ para o modelo em C++)
Abra Plugins/MeuPlugin/Source/MeuPlugin.c e escreva as funções
Rode build.bat (precisa do Visual Studio com C++), reinicie o editor
Os nós aparecem na categoria Plugins dos Blueprints e nos scripts
```
Um **plugin** é uma pasta com um `plugin.json` e uma DLL. Fica em `<projeto>/Plugins/<Nome>`, ou em `Plugins/` ao lado da engine. Na abertura, a engine carrega os plugins ativos, e cada um registra o que traz pelo `plugin_api.h`, um cabeçalho C só:
- **Nós de Grafo:** **puros** (um valor, sem pinos de execução) ou **chamáveis** (Exec de entrada e saída), com até 8 entradas e 4 saídas de float, int, bool, vector, rotator ou color. O mesmo nó vira função nos scripts: "Smooth Step" é `SmoothStep(...)` em Rivora Script, C#, Python e C.
- **Tick:** uma função chamada a cada quadro, no editor e no jogo.
- **Menu:** itens em Ferramentas > Plugins (só no editor).
**Editar > Plugins** mostra todos como a Unreal: uma barra lateral com Todos, Embutidos, Projeto e as categorias (Artista, Level Design, Animação, Gameplay, Produtividade, Render, Áudio, IA, UI, Online), cada uma com a contagem; e um cartão por plugin com o ícone, o nome, a versão, o autor, a descrição, **Ativado** e o estado (carregado, "precisa reiniciar", com erro; passe o mouse para ver os nós). Em cima ficam a busca, **Novo Plugin...** e **Abrir Pasta**. Um plugin de DLL muda na próxima abertura, como na Unreal; a escolha fica em `Config/Plugins.ini`. No `plugin.json`, `Category` põe o plugin numa categoria e `Icon` escolhe um dos ícones da engine pelo nome (`"BRUSH"`, `"CAMERA"`, `"SOUND"`...); sem ele, o cartão usa o ícone da categoria. Os 32 plugins embutidos do editor estão no artigo "Plugins embutidos do editor".
**Segurança:** se uma função do plugin travar (acesso inválido), o plugin é desligado e o editor continua.
**Empacotar o jogo** leva os plugins carregados para `Plugins/` do pacote.
O **Example Plugin** (ao lado da engine, desligado de fábrica) mostra tudo isso e traz o código-fonte em `Plugins/ExamplePlugin/Source`.
```c
#include "plugin_api.h"
static void dobro(const RePluginValue* in, RePluginValue* out, void* user) {
    out[0].v[0] = in[0].v[0] * 2.0f;
}
RE_PLUGIN_EXPORT int re_plugin_init(const RePluginApi* api) {
    api->add_function(api->plugin, "Double It", "Math", "O valor vezes 2.",
                      "i:float:Value,o:float:Return Value", true, dobro, 0);
    return 0;
}
```
Ao **trocar de projeto**, os plugins do projeto que fecha são desligados: a função de shutdown de cada um roda, e os nós, os ticks e os itens de menu dele saem. Os plugins da engine continuam.

## Gameplay Framework (plugin embutido)
Os módulos de jogo prontos (Vida e dano, Inventário, Interação, Menu de Pausa e Opções, Salvar o Progresso, Diálogo, Missões, Regras de Jogo, Habilidades e o HUD) formam o plugin **Gameplay Framework**, que vem ligado. Em **Editar > Plugins** você o desliga para o projeto: os nós, componentes, atores (Pickup, Checkpoint, Game Rules, portas e baús) e assets (Diálogo, Missão) somem dos menus, e o Play não cria nada dele sozinho (o Interactor do jogador, o aviso de interação, o HUD, a tecla do menu de pausa). Por ser da engine, liga e desliga na hora, sem reiniciar. A escolha fica em `Config/Plugins.ini` e vai junto no jogo empacotado.
--- en
```diagram steps
Edit > Plugins > New Plugin...: type the name (e.g. MyPlugin; tick C++ for the C++ template)
Open Plugins/MyPlugin/Source/MyPlugin.c and write the functions
Run build.bat (needs Visual Studio with C++), restart the editor
The nodes show up in the Blueprints' Plugins category and in the scripts
```
A **plugin** is a folder with a `plugin.json` and a DLL. It lives in `<project>/Plugins/<Name>`, or in `Plugins/` next to the engine. At startup the engine loads the enabled plugins, and each registers what it brings through `plugin_api.h`, a single C header:
- **Graph nodes:** **pure** (a value, no execution pins) or **callable** (Exec in and out), with up to 8 inputs and 4 outputs of float, int, bool, vector, rotator or color. The same node becomes a function in the scripts: "Smooth Step" is `SmoothStep(...)` in Rivora Script, C#, Python and C.
- **Tick:** a function called every frame, in the editor and in the game.
- **Menu:** items under Tools > Plugins (editor only).
**Edit > Plugins** shows them all as Unreal does: a side bar with All, Built-in, Project and the categories (Artist, Level Design, Animation, Gameplay, Productivity, Rendering, Audio, AI, UI, Online), each with its count; and a card per plugin with its icon, name, version, author, description, **Enabled** and its state (loaded, "restart needed", failed; hover it for its nodes). At the top are the search, **New Plugin...** and **Open Folder**. A DLL plugin changes at the next start, as in Unreal; the choice is kept in `Config/Plugins.ini`. In `plugin.json`, `Category` puts the plugin in a category and `Icon` picks one of the engine's icons by name (`"BRUSH"`, `"CAMERA"`, `"SOUND"`...); without it the card uses its category's icon. The editor's 32 built-in plugins are in the article "The editor's built-in plugins".
**Safety:** if a plugin's function crashes (an invalid access), the plugin is turned off and the editor goes on.
**Package Game** takes the loaded plugins into the package's `Plugins/`.
The **Example Plugin** (next to the engine, off by default) shows all of this and has its source code in `Plugins/ExamplePlugin/Source`.
```c
#include "plugin_api.h"
static void twice(const RePluginValue* in, RePluginValue* out, void* user) {
    out[0].v[0] = in[0].v[0] * 2.0f;
}
RE_PLUGIN_EXPORT int re_plugin_init(const RePluginApi* api) {
    api->add_function(api->plugin, "Double It", "Math", "The value times 2.",
                      "i:float:Value,o:float:Return Value", true, twice, 0);
    return 0;
}
```
When you **switch projects**, the closing project's plugins are turned off: each one's shutdown runs, and its nodes, ticks and menu items go away. The engine's plugins stay.

## Gameplay Framework (built-in plugin)
The ready-made game modules (Health and damage, Inventory, Interaction, Pause Menu and Options, Save Progress, Dialogue, Quests, Game Rules, Abilities and the HUD) make up the **Gameplay Framework** plugin, on by default. In **Edit > Plugins** you turn it off for the project: its nodes, components, actors (Pickup, Checkpoint, Game Rules, doors and chests) and assets (Dialogue, Quest) leave the menus, and Play makes none of its pieces by itself (the player's Interactor, the interaction prompt, the HUD, the pause menu's key). Being the engine's, it turns on and off at once, no restart. The choice lives in `Config/Plugins.ini` and goes with the packaged game.
--- es
```diagram steps
Editar > Plugins > Nuevo Plugin...: escribe el nombre (p. ej. MyPlugin; marca C++ para la plantilla C++)
Abre Plugins/MyPlugin/Source/MyPlugin.c y escribe las funciones
Ejecuta build.bat (necesita Visual Studio con C++), reinicia el editor
Los nodos aparecen en la categoría Plugins de los Blueprints y en los scripts
```
Un **plugin** es una carpeta con un `plugin.json` y una DLL. Vive en `<proyecto>/Plugins/<Nombre>`, o en `Plugins/` junto al motor. Al iniciar, el motor carga los plugins habilitados, y cada uno registra lo que trae mediante `plugin_api.h`, un único header en C:
- **Nodos de Grafo:** **puros** (un valor, sin pines de ejecución) o **invocables** (Exec de entrada y salida), con hasta 8 entradas y 4 salidas de tipo float, int, bool, vector, rotator o color. El mismo nodo se convierte en una función en los scripts: "Smooth Step" es `SmoothStep(...)` en Rivora Script, C#, Python y C.
- **Tick:** una función llamada en cada cuadro, en el editor y en el juego.
- **Menú:** ítems en Herramientas > Plugins (solo en el editor).
**Editar > Plugins** los muestra todos como Unreal: una barra lateral con Todos, Integrados, Proyecto y las categorías (Artist, Level Design, Animation, Gameplay, Productivity, Rendering, Audio, AI, UI, Online), cada una con su cuenta; y una tarjeta por plugin con su ícono, nombre, versión, autor, descripción, **Enabled** y su estado (cargado, "reinicio necesario", falló; pasa el ratón por encima para ver sus nodos). Arriba están la búsqueda, **Nuevo Plugin...** y **Abrir carpeta**. Un plugin de DLL cambia en el próximo inicio, como en Unreal; la elección se guarda en `Config/Plugins.ini`. En `plugin.json`, `Category` pone el plugin en una categoría e `Icon` elige uno de los íconos del motor por nombre (`"BRUSH"`, `"CAMERA"`, `"SOUND"`...); sin él la tarjeta usa el ícono de su categoría. Los 32 plugins integrados del editor están en el artículo "Los plugins integrados del editor".
**Seguridad:** si la función de un plugin falla (un acceso inválido), el plugin se apaga y el editor sigue.
**Empaquetar juego** lleva los plugins cargados a `Plugins/` del paquete.
El **Example Plugin** (junto al motor, desactivado por defecto) muestra todo esto y tiene su código fuente en `Plugins/ExamplePlugin/Source`.
```c
#include "plugin_api.h"
static void twice(const RePluginValue* in, RePluginValue* out, void* user) {
    out[0].v[0] = in[0].v[0] * 2.0f;
}
RE_PLUGIN_EXPORT int re_plugin_init(const RePluginApi* api) {
    api->add_function(api->plugin, "Double It", "Math", "The value times 2.",
                      "i:float:Value,o:float:Return Value", true, twice, 0);
    return 0;
}
```
Al **cambiar de proyecto**, los plugins del proyecto que se cierra se desactivan: se ejecuta el shutdown de cada uno, y sus nodos, ticks y elementos de menú desaparecen. Los plugins del motor siguen.

## Gameplay Framework (plugin integrado)
Los módulos de juego ya hechos (Salud y daño, Inventario, Interacción, Menú de pausa y Opciones, Guardar progreso, Diálogo, Misiones, Reglas de juego, Habilidades y el HUD) forman el plugin **Gameplay Framework**, activado por defecto. En **Editar > Plugins** lo desactivas para el proyecto: sus nodos, componentes, actores (Pickup, Checkpoint, Game Rules, puertas y cofres) y assets (Dialogue, Quest) salen de los menús, y Jugar no crea ninguna de sus piezas por sí solo (el Interactor del jugador, el aviso de interacción, el HUD, la tecla del menú de pausa). Al ser del motor, se activa y desactiva al instante, sin reiniciar. La elección vive en `Config/Plugins.ini` y va con el juego empaquetado.
