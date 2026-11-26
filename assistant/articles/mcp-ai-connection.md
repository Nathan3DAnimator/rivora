id: mcp-ai-connection
kind: howto
title: Conectar uma IA ao editor (MCP: Claude, Cursor) | Connect an AI to the editor (MCP: Claude, Cursor) | Conectar una IA al editor (MCP: Claude, Cursor)
keys: mcp, model context protocol, ia, ai, inteligencia artificial, artificial intelligence, claude, claude code, claude desktop, cursor, vs code, copilot, assistente de ia, ai assistant, agente, agent, automatizar, automate, controlar o editor, control the editor, conexao com ia, ai connection, rivora_mcp, servidor, server, porta, port, 47860, chatgpt, llm
--- pt
```diagram steps
Ferramentas > Conexão com IA (MCP) | Ligar Servidor MCP | Copiar a configuração do cliente | A IA chama get_started
```
## Ligar
- **Ferramentas > Conexão com IA (MCP)** e marque **Ligar Servidor MCP**. Ele fica ligado nas próximas vezes.
- O servidor só aceita programas deste computador (127.0.0.1; páginas da web são recusadas).

## Conectar o cliente
- **Claude Code**: copie o comando da janela (`claude mcp add --transport http rivora http://127.0.0.1:47860/mcp`) e rode no terminal.
- **Claude Desktop**: copie o JSON da janela para o `claude_desktop_config.json`. Ele usa o `rivora_mcp.exe`, que **abre o editor sozinho** quando ele não está aberto.
- **Cursor, VS Code e outros**: use a URL `http://127.0.0.1:47860/mcp`.

## O que a IA consegue fazer
- Ver o nível: estado do editor, atores (filtros, distância), um ator com todas as propriedades do Details.
- Mudar em lote: **colocar**, **modificar** e **apagar** muitos atores numa chamada (tipo da lista do Adicionar ou um asset do Content).
- **Rodar scripts** (Python, Rivora Script, C# ou C) no nível com a API inteira da engine; os `print` e os erros com a linha voltam para ela.
- **Escrever classes** de gameplay em `/Content`, compilar e colocar no nível.
- Mover a câmera e **tirar foto** da viewport, dar **Play** por alguns segundos, mudar variáveis do console, salvar e abrir níveis.
- Ler este Assistente e a referência da API.

Tudo o que a IA faz se desfaz com **Ctrl+Z**, e nada é salvo até alguém salvar o nível. Peça para ela chamar **get_started** primeiro: ali estão as convenções (cm, Z para cima, graus) e o jeito mais rápido de trabalhar.
--- en
```diagram steps
Tools > AI Connection (MCP) | Enable MCP Server | Copy the client's setup | The AI calls get_started
```
## Turn it on
- **Tools > AI Connection (MCP)** and check **Enable MCP Server**. It stays on the next times.
- The server only accepts programs on this computer (127.0.0.1; web pages are refused).

## Connect the client
- **Claude Code**: copy the command from the window (`claude mcp add --transport http rivora http://127.0.0.1:47860/mcp`) and run it in a terminal.
- **Claude Desktop**: copy the JSON from the window into `claude_desktop_config.json`. It uses `rivora_mcp.exe`, which **opens the editor by itself** when it isn't running.
- **Cursor, VS Code and others**: use the URL `http://127.0.0.1:47860/mcp`.

## What the AI can do
- See the level: the editor's state, the actors (filters, distance), one actor with every Details property.
- Change in batches: **place**, **modify** and **delete** many actors in one call (a type from the Add list or a Content asset).
- **Run scripts** (Python, Rivora Script, C# or C) on the level with the engine's whole API; the prints and the errors with their lines come back to it.
- **Write gameplay classes** in `/Content`, compile them and place them.
- Move the camera and **take a picture** of the viewport, **Play** for a few seconds, change console variables, save and open levels.
- Read this Assistant and the API reference.

Everything the AI does can be undone with **Ctrl+Z**, and nothing is saved until someone saves the level. Ask it to call **get_started** first: it has the conventions (cm, Z up, degrees) and the fastest way to work.
--- es
```diagram steps
Herramientas > AI Connection (MCP) | Enable MCP Server | Copia la configuración del cliente | La IA llama a get_started
```
## Actívalo
- **Herramientas > AI Connection (MCP)** y marca **Enable MCP Server**. Queda activado las próximas veces.
- El servidor solo acepta programas de este equipo (127.0.0.1; las páginas web son rechazadas).

## Conecta el cliente
- **Claude Code**: copia el comando de la ventana (`claude mcp add --transport http rivora http://127.0.0.1:47860/mcp`) y ejecútalo en una terminal.
- **Claude Desktop**: copia el JSON de la ventana en `claude_desktop_config.json`. Usa `rivora_mcp.exe`, que **abre el editor por sí solo** cuando no está en ejecución.
- **Cursor, VS Code y otros**: usa la URL `http://127.0.0.1:47860/mcp`.

## Qué puede hacer la IA
- Ver el nivel: el estado del editor, los actores (filtros, distancia), un actor con todas las propiedades de Detalles.
- Cambiar en lotes: **colocar**, **modificar** y **eliminar** muchos actores en una llamada (un tipo de la lista Añadir o un asset de Content).
- **Ejecutar scripts** (Python, Rivora Script, C# o C) en el nivel con toda la API del motor; los prints y los errores con sus líneas le regresan.
- **Escribir clases de gameplay** en `/Content`, compilarlas y colocarlas.
- Mover la cámara y **tomar una foto** de la ventana gráfica, **Jugar** durante unos segundos, cambiar variables de consola, guardar y abrir niveles.
- Leer este Asistente y la referencia de la API.

Todo lo que hace la IA se puede deshacer con **Ctrl+Z**, y nada se guarda hasta que alguien guarde el nivel. Pídele que llame primero a **get_started**: trae las convenciones (cm, Z arriba, grados) y la forma más rápida de trabajar.
