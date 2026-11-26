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

## Guia que acelera a IA (2026-11-24, item 6: `src/mcp_guide.inc`, incluído pelo mcp_tools.c)
- **Checagem dos argumentos** contra o `inputSchema` de cada ferramenta antes de rodar (chaves, tipos, enums, required;
  um nível dentro de listas de objetos): chave errada responde a certa (`position` -> "did you mean 'location'?", tabela
  de sinônimos + prefixo) e a lista do que a ferramenta aceita; sempre aponta `tool_help`.
- **batch**: passos `{tool, args}` (só ferramentas do mundo: spawn/modify/delete/select, set_camera, run_script e as de
  leitura) num **passo de desfazer**: `editor_undo_push` uma vez e `ed->undo_hold` segura os pushes de dentro; atômico
  (padrão) ou `dry_run` revertem com `editor_undo_revert` (aplica e descarta o passo, sem redo). Passo malformado ou
  ferramenta que grava arquivo/joga/espera quadros: recusado antes de rodar qualquer coisa. A câmera não volta (não
  está no desfazer).
- **recipe**: test_level, lighting (os Sky Presets), playable_character (classe Character pelo `bp_create`, a câmera
  como nos templates, Default Pawn + `project_save`, Player Start se faltar), pbr_material (mapas achados pelo nome
  na pasta no disco: BaseColor/Albedo/Diffuse/Color, Roughness, Metallic, Normal, AO; grafo como o do kit das demos:
  AO multiplica a cor, Normal Map), hud (HUD Layout num ator GameHUD). Cada uma gera os passos de um batch (o
  `dry_run` responde o plano); os arquivos que ela cria (classe, material) ficam depois de um desfazer.
- **inspect_level**: luzes, chão, jogador, atores caídos (Z < -100 m), scripts que não compilam, erros no log, quadro
  lento, não salvo; cada um com o conserto; `next_steps`.
- **tool_help** (schema + exemplos que funcionam + erros comuns + relacionadas), **search** (tipos, componentes,
  receitas, atores, assets, funções, artigos), **`next`** no fim de toda resposta (`add_next`), `components` no
  spawn/modify (como Details > Add), recursos `rivora://recipes` e `rivora://state`, prompt `make_playable`.

- **Medido** (teste `mcp`, RTX 3060, editor fora da tela): nível de teste com as ferramentas antigas (get_started,
  list_actor_types, spawn_actors, set_camera) **4 chamadas, 24,0 ms, 6074 bytes lidos pela IA**; com
  `recipe test_level` **1 chamada, 6,4 ms, 1078 bytes** (8 atores, um Ctrl+Z). Saber o que está errado: antes 3
  chamadas (estado, atores, log) 29,2 ms e 6136 bytes que a IA ainda precisa interpretar; `inspect_level` 1 chamada,
  7,8 ms, 629 bytes com cada problema e o conserto. Material PBR, personagem jogável e HUD não tinham ferramenta
  (exigiam run_script e várias consultas à api_reference); agora uma chamada cada. tools/list: 27 ferramentas,
  ~15,8 KB pela ponte stdio (era 12,2 KB).
- Teste `mcp` estendido: chave errada (position), enum errado, tool_help, a medida acima, um Ctrl+Z desfaz a receita,
  dry_run não muda nada, estilo errado, batch atômico revertido, batch recusa `level`, batch de 3 passos com
  `components` = um passo de desfazer, pbr_material (3 mapas achados pelo nome, material aplicado), playable_character
  (primeira pessoa, Default Pawn visto pelo inspect_level), hud, search, recursos `rivora://recipes` e `rivora://state`.

## Ferramentas
recipe, batch, inspect_level, tool_help, search (2026-11-24); get_started, get_editor_state, find_actors, get_actor, spawn_actors, modify_actors, delete_actors, select_actors,
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
- **Uma chamada ida e volta** (HTTP, fila, até um quadro, resposta): 2,5-3,0 ms (`get_editor_state`, média de 20;
  RTX 3060, editor a ~350 fps). O servidor não custa nada no quadro sem chamadas (só o teste da fila vazia).
- `run_script` em Python (laço de 3 com print): compilar + rodar 0,17-0,20 ms.
- `screenshot` 320 px: 3 quadros de espera + a cópia + PNG em memória (base64), resposta com a imagem.
- `tools/list`: 22 ferramentas, ~12 KB (o initialize + a lista pela ponte stdio: 12,2 KB).
- `rivora_mcp.exe`: 15 KB.
- Pendente: conferir com um cliente MCP oficial (SDK/Inspector): exige baixar o SDK; o teste usa um cliente HTTP
  próprio seguindo a spec e a ponte stdio.

### Ensaio (dry_run) das receitas e contagem de atores (25/11/2026)
- `dry_run` monta o plano inteiro sem tocar no disco nem no projeto: a classe do `playable_character` existe só em memória
  durante o ensaio (liberada no rollback), a `hud` não grava o `WBP_GameHUD.rbp`, o `set_camera` do `test_level` volta.
  A resposta traz `actors_would_be` (quantos atores a chamada real deixaria).
- `actors_after` é contado depois do `anim_update` (as partes do Mannequin nascem no quadro seguinte).
- O Ctrl+Z da receita leva os atores, não as Project Settings: a resposta e o `next` avisam que o Default Pawn continua
  apontando para a classe criada (o desfazer é um retrato do mundo).
- Teste `mcp` (recipe_dry + recipe_real): actors_after = atores logo depois = actors_would_be (test_level 22, pbr_material
  15, playable_character 49, hud 50).
