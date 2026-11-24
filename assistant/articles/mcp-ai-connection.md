id: mcp-ai-connection
kind: howto
title: Conectar uma IA ao editor (MCP: Claude, Cursor) | Connect an AI to the editor (MCP: Claude, Cursor)
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
- **Rodar scripts** (Python, Real, C# ou C) no nível com a API inteira da engine; os `print` e os erros com a linha voltam para ela.
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
- **Run scripts** (Python, Real, C# or C) on the level with the engine's whole API; the prints and the errors with their lines come back to it.
- **Write gameplay classes** in `/Content`, compile them and place them.
- Move the camera and **take a picture** of the viewport, **Play** for a few seconds, change console variables, save and open levels.
- Read this Assistant and the API reference.

Everything the AI does can be undone with **Ctrl+Z**, and nothing is saved until someone saves the level. Ask it to call **get_started** first: it has the conventions (cm, Z up, degrees) and the fastest way to work.
