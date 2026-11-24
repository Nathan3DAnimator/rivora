# MCP: a IA controlando o editor (2026-11-24)

Pedido do usuário: "suporte ao MCP, criando uma API que já ajude a guiar e acelerar o processo da IA que tiver no
controle". O editor tem um servidor do **Model Context Protocol**: Claude Code, Claude Desktop, Cursor ou qualquer
cliente MCP vê e muda o nível pelo editor aberto.

## Arquivos
| Arquivo | O quê |
|---|---|
| `src/mcp.h` | A API: ligar/desligar, a porta, o quadro da thread principal, a janela, as preferências |
| `src/mcp_server.c` | Transporte e protocolo: HTTP em 127.0.0.1, JSON-RPC, a fila para a thread principal |
| `src/mcp_tools.c` | As 22 ferramentas, o guia (`get_started`, `instructions`), prompts e recursos |
| `src/mcp_ui.c` | Ferramentas > Conexão com IA (MCP); `mcp_enabled` / `mcp_port` no `editor.ini` |
| `src/mcp_bridge.c` | `rivora_mcp.exe`: o transporte stdio (Claude Desktop); abre o editor quando ele não está aberto |
| `src/mcp_test.c` | `--mcp-test` (teste `mcp`) |

## Transporte
- **Streamable HTTP** (spec 2025-06-18; aceita 2024-11-05, 2025-03-26 e 2025-11-25): `POST /mcp` com uma mensagem,
  resposta `application/json` (sem SSE: `GET /mcp` dá 405), notificação = 202, `Mcp-Session-Id` devolvido.
- Porta padrão **47860** (Preferências / janela, `--mcp-port`). `SO_EXCLUSIVEADDRUSE`: um segundo editor não toma a
  porta do primeiro (a janela mostra "porta em uso").
- **Segurança**: só `127.0.0.1`; `Host` e `Origin` precisam ser deste computador (uma página da web que chega em
  127.0.0.1 por DNS rebinding leva 403). Desligado até o usuário ligar (`--mcp` liga só naquela vez). Os testes nunca
  ligam pelo `editor.ini` do usuário.
- **stdio**: o `rivora_mcp.exe` (poucos KB, sem nada do editor) manda cada linha do stdin como um POST e escreve a
  resposta como uma linha. Sem editor na porta: abre o `rivora.exe` ao lado com `--mcp --mcp-port N` e espera até 90 s;
  sem conseguir, responde um erro JSON-RPC dizendo o que fazer. `--no-launch` e `RIVORA_MCP_PORT` existem.

## Threads
- Uma thread escuta; cada conexão tem a sua (keep-alive). `initialize`, `tools/list`, `prompts/*`,
  `resources/list` respondem na própria thread (dados fixos).
- `tools/call` e `resources/read` vão para uma fila; `mcp_server_frame` (main.c, depois do `editor_frame`, dentro do
  quadro da UI) roda até ~6 ms de chamadas por quadro. Uma chamada pode levar vários quadros (`screenshot`: 3 quadros e
  a cópia; `play` com segundos). A thread da conexão espera até 10 min; desistindo, a principal libera a chamada quando
  terminar (sem corrida: estado sob uma seção crítica).

## Como ela guia e acelera a IA
- `instructions` do `initialize` e a ferramenta **get_started**: convenções (cm, Z para cima, graus, ids), o fluxo mais
  rápido, os tipos de ator, exemplos de script, o estado do nível e quantos atores de cada classe.
- **Lotes**: `spawn_actors`, `modify_actors`, `delete_actors` levam listas (uma cena inteira numa chamada).
- **Erros que dizem o próximo passo**: tipo errado -> os tipos parecidos; propriedade errada -> as do ator com nome
  parecido; erro de script -> linha (do código dela, sem o cabeçalho que o servidor põe) e o artigo do Assistente que
  explica a mensagem (`help_doc`).
- **Respostas curtas**: números com uma casa, só o que mudou do padrão (rotação zero e escala 1 omitidas), páginas.
- **run_script**: o corpo do código vira uma classe `MCP_Run` (pai EditorUtility, evento Run) compilada em memória (nada
  no disco) e rodada no nível (`bp_run_editor_event`, um passo de desfazer); os `print` voltam pelo
  `bp_set_message_sink` (bp_vm.c). A API inteira dos scripts fica disponível; `api_reference` dá as assinaturas.
- **write_script**: classes de gameplay em `/Content`, compiladas na hora (erros com linha), colocadas por
  `spawn_actors` (asset).
- `screenshot` só quando a aparência importa (custa imagem); `find_actors` / `get_actor` respondem o resto.
- Prompts prontos no cliente: build_scene, gameplay_feature, optimize_performance, fix_problem. Recursos:
  `rivora://guide`, `rivora://docs/{id}`.

## Ferramentas
get_started, get_editor_state, find_actors, get_actor, spawn_actors, modify_actors, delete_actors, select_actors,
set_camera, screenshot, run_script, write_script, api_reference, search_docs, read_doc, list_assets, list_actor_types,
console, play, level, undo, get_log.
- Tipos para `spawn_actors`: o catálogo do Adicionar (`editor_place_count/name/spawn`, editor.c), com apelidos (Box,
  Light, Sun, Camera, Fog...); Sky Presets aplicam e respondem `applied`. Assets: Static Mesh, Skeletal Mesh, Blueprint,
  script, Particle System, Vehicle (o mesmo que soltar na viewport).
- Propriedades: os nomes do Details (`props.h`, o mesmo do Set Property by Name).

## Teste `mcp` (`--mcp-test`, área mcp no kit rápido)
Uma thread fala HTTP de verdade com o servidor (porta 47871) enquanto o editor roda: initialize, notificação 202,
tools/list, prompts, recurso, Origin estranho = 403, GET = 405; get_started, spawn em lote com um tipo errado, modify
(offset, cor, ator que não existe), get_actor, find, run_script em Python (prints) e em Real com erro na linha 1,
api_reference, console, set_camera + screenshot 320 px, delete + undo, Play por 1 s, ferramenta desconhecida; e o
`rivora_mcp.exe` por pipes (initialize + tools/list = 2 linhas).

## Medidas
(preenchidas pelo teste: tempo de uma chamada ida e volta, screenshot)
