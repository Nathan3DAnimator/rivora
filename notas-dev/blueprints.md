# Blueprints (programação visual)

Como os Blueprints da Unreal: assets na pasta Content, editados como grafos de nodes, compilados para um bytecode por
registradores que uma VM roda sem alocar nada.

## Arquivos
- `src/blueprint.h/.c` — os assets, o registro dos nodes (nome, categoria, dica, pinos), edição do grafo, arquivos
  JSON, o template do Character. `src/bp_internal.h` — os tipos de node (`BPK_*`) e os opcodes.
- `src/bp_vm.c` — o compilador, a VM, o runtime (instâncias, eventos, Delay, interfaces, Spawn), o movimento do
  Character (cápsula, gravidade, pulo, degrau de 45 cm, colisão com as caixas das malhas numa grade de 10 m) e a câmera.
- `src/bp_editor.c` — o Blueprint Editor (janela por asset). `src/bp_test.c` — o teste.
- Componentes: `CBlueprint` (o hash do asset + a instância durante o jogo), `CCharacter` (Character Movement).

## Assets (Content Browser > Adicionar, ou clique direito > Criar Asset Avançado)
| Extensão | O quê |
|---|---|
| `.rbp` | Classe Blueprint: pai (Actor, Pawn, Character, Game Mode ou outro Blueprint), componentes, variáveis, interfaces, Event Graph e funções |
| `.rstruct` | Estrutura: campos (uma variável dela ocupa um registrador por campo) |
| `.renum` | Enumeração: entradas (Switch on, Literal) |
| `.rinterface` | Interface: funções que qualquer classe implementa; chamadas com "(Message)" em qualquer ator |
| `.rdata` | Data Asset: valores lidos com "Get Data Asset Value" (constantes na compilação) |

O nível guarda o hash do caminho virtual (`/Content/...`). Renomear/mover no Content Browser atualiza o asset, quem o
cita pelo nome e (na sessão) o hash antigo. Arrastar uma classe para o viewport cria o ator. Duplo clique abre o editor.
Clique direito numa classe > **Criar Classe Blueprint Filha** (`bp_editor_create_child`): `<Nome>_Child` na mesma pasta,
com o pai e os componentes dele (teste `blueprints` cria e compila uma). No Content Browser cada classe mostra a miniatura
(a mesma prévia do Viewport) com o selo do tipo; sem nada para ver, o ícone do tipo (Actor, Pawn, Character, Game Mode);
estruturas, enums, interfaces e Data Assets têm ícones próprios na cor do tipo. O menu Adicionar > Classe Blueprint mostra
as Classes Comuns com ícone e dica (como o Pick Parent Class da Unreal) e "Filha de" cada classe do projeto.

## O layout (o da Unreal)
- Barra: **Compilar** (selo: ✓ compilado, ? não compilado, ✗ erro), Salvar, **Localizar** (mostra no Content Browser),
  Configurações da Classe (pai, interfaces), **Padrões da Classe** (todos os componentes), Jogar; "Classe pai" à direita.
- Esquerda: **Componentes** (+ Adicionar; a classe (Próprio), a raiz — DefaultSceneRoot, ou CapsuleComponent num
  Character —, Mesh/StaticMesh, luz, os movimentos; clique mostra o componente em Detalhes, clique direito apaga) e
  **Meu Blueprint** (Grafos, Funções +, Variáveis + com a cor e o tipo, Interfaces).
- Centro: abas **Viewport**, Gráfico de Eventos e uma por função; embaixo os Resultados do Compilador. Direita: Detalhes.
- **Viewport** (`bp_preview.c`; desde 2026-11-03 renderizado pela GPU com materiais e modos de visualização, `editor.md`; o texto abaixo é o desenho na CPU, que ficou para as miniaturas): a malha
  com GGX + luz chave do canto da câmera + céu/chão + recorte, sombreamento suave (Gouraud), contorno com linhas
  suavizadas (adjacência por posição: a costura da UV não vira contorno), ACES; grade que some na distância, sombra de
  contato; de um Character a cápsula (a metade de trás apagada), a seta e a câmera no braço; de uma luz a lâmpada.
  Orbitar: arrastar; mover: botão do meio; zoom: roda; F enquadra. Abre no Viewport uma classe sem script, senão no grafo.
- Nodes: cabeçalho arredondado com degradê da cor do tipo e ícone (evento, função, interface, cast), sombra suave,
  contorno laranja na seleção e claro no hover, pinos com a setinha do fluxo, fios com borda escura, Get de variável
  compacto (pílula na cor do tipo), grade da Unreal, marca d'água BLUEPRINT/FUNCTION, trilha "Classe > Grafo" e o zoom.

## O editor (atalhos da Unreal)
- Arrastar de um pino e soltar no vazio: só os nodes que encaixam no fio, ligados ao serem criados (Context Sensitive).
- Clique direito: todos os nodes, busca por palavras, Enter cria o primeiro. B/S/D + clique: Branch, Sequence, Delay.
- Alt + clique no pino: desfaz as ligações. Delete, Ctrl+D, Ctrl+A, Home enquadra, F7 compila, Ctrl+S salva.
- Variáveis arrastadas de My Blueprint (Ctrl: Get, Alt: Set). Valores digitados nos pinos soltos.
- Erro de compilação: o node fica vermelho, selecionado e centralizado. Jogar (Alt+P) salva e compila o que mudou.
- Salvar uma classe aplica os padrões (componentes) aos atores dela no nível (com desfazer).

## Componentes (a árvore da Unreal, `bp_components.c`)
- A classe tem uma árvore de componentes (`BpAsset.scs`, até 32; o pai sempre vem antes do filho): Scene, Static Mesh,
  Point/Spot/Rect Light, Camera, Spring Arm, Box/Sphere/Capsule Collision, Arrow. "+ Adicionar" agrupa como a Unreal
  (Comuns, Luzes, Câmera, Colisão, Movimento) e põe o novo debaixo do selecionado. Movimento fica no próprio ator
  (`comp_data`): Character Movement, Rotating, **Projectile Movement** (lança pelo X do ator, gravidade, quica/para) e
  **Floating Pawn Movement** (o Add Movement Input de um Pawn sem Character).
- No nível cada nó vira uma entidade filha (`CAttach`: dono, pai, transformação relativa, índice do nó).
  `bp_components_update` (editor e jogo) passa por profundidade (nó 0 de todos, depois 1...) e compõe pai ∘ relativo;
  sem componentes no nível custa só a varredura dos arquétipos. Filhos de ator apagado somem no quadro seguinte. A
  escala do ator passa para os filhos, menos num Character (a escala dele é o tamanho do corpo).
- **Spring Arm**: começo = posição + Target Offset, volta pelo X (ou pela rotação de controle, "Use Pawn Control
  Rotation") pelo comprimento, + Socket Offset; jogando: teste de colisão (`bp_trace_boxes`, esfera do Probe Size
  contra as caixas do jogo) e Camera Lag. A entidade do braço fica na ponta: o que pendura nele (a câmera) segue.
- A câmera do jogador: a primeira Camera do pawn possuído; sem ela, o `camera_distance` antigo. Um Character novo
  nasce como o template Third Person: Arrow, CameraBoom (350 cm, rotação de controle) e FollowCamera.
- Colisões com "Bloquear Tudo" viram caixas do jogo (Characters e projéteis batem); malhas de componentes não
  bloqueiam (a de um Character o seguraria).
- No nível: o Organizador não lista os componentes, clicar num deles seleciona o ator, Ctrl+D refaz os do clone,
  abrir um nível/salvar a classe refaz todos (`bp_components_rebuild`).
- Viewport do BP: clique seleciona (malhas por raio contra a caixa, ajudantes por 12 px), gizmo **W/E/R** (mover e
  girar nos eixos do pai, onde os valores vivem; escalar nos do próprio nó; meio = uniforme; Ctrl encaixa 10 cm /
  15° / 0,1), Delete apaga. As malhas são desenhadas de trás para frente (cada uma convexa).

## Compilador e VM
- Registradores por instância: 0 = self, as variáveis, as saídas dos nodes (struct = um por campo). Constantes à
  parte. Nodes puros são reavaliados onde são usados (regra da Unreal); a cadeia de execução vira código com JMP; um
  node alcançado por dois caminhos tem uma cópia só.
- Sequence/For Loop/funções: pilha de retorno (32). Delay: slot latente por instância, retoma no pc salvo.
- Herança: a compilação junta as variáveis e grafos da cadeia de pais; o evento do filho substitui o do pai.
- Orçamento de 1 000 000 ops por execução: loop infinito é parado com mensagem (como a Unreal).
- Memória: instâncias e registradores numa arena reservada no Play (sem malloc no frame).

## Medidas (RTX 3060, 1440p)
- Teste `blueprints`: ator de teste 44 ops / 34 registradores, Character 18 ops; BeginPlay de 2 atores 0.18–0.28 ms
  (inclui compilar); Tick dos 2: **0.002 ms**, 17 ops/frame. Editor no teste: 522 fps.
- Editor (nível padrão, 3 s): sem janela 493 fps · CPU 0,56 ms; grafo do BP_TestActor aberto 499 fps · 0,54 ms; **Viewport do
  Character aberto 502 fps · 0,55 ms**; Content Browser na pasta com 7 Blueprints (miniaturas) 604 fps · 0,56 ms. A prévia
  e as miniaturas ficam no ruído da medida (a GPU cai porque as janelas cobrem parte do viewport do nível).
- `game_blueprints`: o mesmo nível empacotado, o jogo roda os Blueprints (BeginPlay imprime) acima de 350 fps.

## Limites conhecidos
- (Feito em 2026-11-08: valor por ator, ver "Instance Editable por ator".)
- Textos montados com Append, To String e Format Text (tabela de strings da VM, sem malloc no quadro).
- Colisão do Character contra caixas alinhadas (AABB das malhas); as caixas são tiradas no Play (Movable não atualiza).
- Recompilar durante o Play não troca as instâncias vivas (a mudança vale no próximo Play).

## Problema conhecido: teste do Enhanced Input intermitente (2026-10-31)
O `blueprints` às vezes termina com `scale z 1.00` no BP_TestInput (cerca de 1 em 5 rodadas da suíte completa;
quase nunca isolado). O diagnóstico impresso na falha mostrou o IA_Move **disparado** (valor Y=1, evento Triggered,
W segurado, jogador possuído) e mesmo assim o evento não mudou a escala do ator. Não reproduziu com o cache de
pipelines frio nem em 12 rodadas seguidas. A falha agora imprime também a instância do Blueprint (viva, quantos
eventos de input) para fechar o diagnóstico na próxima ocorrência.

2026-11-03: são **duas** falhas diferentes.
- **A frequente**, que depois dos merges da 0.15 passou a acontecer quase sempre:
  - Sintoma: `Measure: ... 1 frames in 0.01 s`, OnPing, IA_Move, overlaps e a caminhada falhando juntos.
  - Causa: o `--bench` do editor tinha prazos absolutos contados do início. Quando o aquecimento passava do prazo, a
    medida durava um quadro, e o teste checava um jogo de 0,1 s. O aquecimento passa do prazo com pipelines compilados
    no primeiro quadro: o `pipelines.bin` é um só para todas as worktrees e sessões, chega a ~57 MB de 64 e recomeça
    frio. Também com a GPU disputada.
  - Correção: a janela agora conta a partir do primeiro quadro medido, como o `game_main.c` já fazia.
  - Medida: 3 de 3 rodadas com o cache frio passam.
- **A rara** é a deste parágrafo:
  - Sintoma: `listener scale 0.0` e `scale 1 1 1`. Os eventos rodam, mas leem 0 no parâmetro (Amount, Action Value).
  - Já existia antes dos merges: a build de ea1730d falhou assim uma vez, numa primeira execução com o cache frio.
  - Frequência: ~1 em 8 rodadas na main e em ea1730d, com cache frio ou quente, pacote novo ou existente; também no
    `ctest -R blueprints` isolado.
  - Nas falhas o BP_TestInput tem 664 execuções do evento e a escala continua 1 1 1, a do spawn. O evento roda, mas não
    chega ao Set Scale, ou o Y que ele lê vem 0.
  - O `V.depth` do `run()` está equilibrado (sai pelo `goto done`). Fica em aberto.
  - 2026-11-04 (1 de 8 no `ctest -R blueprints`): o diagnóstico novo da falha diz `1 calls, 1 bound events run, last
    Amount 4.00`. O dispatcher entrega 4, o evento do listener roda uma vez e mesmo assim põe escala 0. O valor se perde
    dentro do evento, entre o `OP_GETARGS` e o Make Vector. O spawn põe escala 1, então o 0 veio de um Set Scale lendo 0.
    O mesmo vale para o IA_Move (Y lido 0). Próximo passo: na falha, imprimir o código da entrada do evento e os
    registradores da instância.

## Editores de Structure, Enumeration e Data Asset como os da Unreal (2026-11-01)
Ver [editor.md](editor.md) ("UI enxuta, descrições..."). Para o compilador: uma variável de estrutura começa com os
**Default Values** da estrutura (campos simples, string e transform; contêineres começam vazios) — antes começava zerada.
A ordem dos Enumerators é o valor de cada um (mover uma entrada muda os valores, como na Unreal).

## Event Dispatchers e Construction Script (2026-11-01)
- **Event Dispatchers** (Unreal): seção no My Blueprint (+ adiciona; clique: as entradas, a assinatura; botão direito:
  os nós Call / Bind / Unbind / Unbind all / Event no grafo aberto). São grafos `BPG_DISPATCHER` (só a assinatura, sem
  nós; salvos como os outros grafos). Nós: `Call <X>` (Target: o ator dono; as entradas viram argumentos via `OP_ARG`),
  `Bind Event to <X>` / `Unbind Event from` / `Unbind all Events from` (Target) e `Event <X>` (saídas = entradas da
  chamada). No menu de ações em "Event Dispatchers" (os da classe e dos pais; de outra classe: `text2` = a classe).
  VM: `OP_DBIND` guarda (ator fonte, nome, instância ouvinte) numa tabela fixa (2048, sem malloc); `OP_DISPATCH` roda o
  `Event <X>` de cada ouvinte ligado com uma cópia dos argumentos. Zerada no fim do Play.
- **Construction Script**: toda classe nova (menos Anim Instance, User Widget e Level Script) ganha a função
  `ConstructionScript`, listada em Graphs (aba "Construction Script"). O compilador a registra também como entrada; ela
  roda quando o ator é feito (início do Play, spawn em jogo, nível em streaming), **antes** de todos os BeginPlay.
  No editor ele roda também (sugestão #64, abaixo).
- Teste `--bp-test`: `BP_TestDispSource` (dispatcher OnPing(Amount) chamado 0,5 s após o BeginPlay; o Construction
  Script o gira para yaw 33) e `BP_TestDispListener` (acha a fonte com Get Actor Of Class, faz Bind, o Event OnPing põe
  a escala em Amount = 4).

## Eventos repetidos, atalhos de fio e Reroute (2026-11-02)
- Dois BeginPlay/Tick/EndPlay na mesma classe rodam os dois, em ordem (antes o segundo substituía o primeiro: o Tick do
  template parava de andar). A entrada vira um stub PUSHRET/JMP para cada um; a filha ainda substitui o da mãe.
- Fio arrastado: B/S/D cria Branch/Sequence/Delay no mouse já ligado; solto no corpo de um node liga no primeiro pino
  livre do mesmo tipo (senão no primeiro que aceite). Duplo clique num fio põe um Reroute nele (o fio sob o mouse
  acende). Zoom aplicado antes de desenhar (nodes e fios piscavam).
- Widget Blueprint sem o painel Components.

## A falha "rara" do bp-test resolvida (2026-11-04)
Com o dump novo (`bp_debug_dump`: código, entradas e registradores da instância na falha), o código do listener que
falhava não tinha o `OP_GETARGS` ou tinha o Make Vector lendo a constante 1 em vez do Amount: **os fios do Event OnPing
sumiam ao ler os assets do disco**. A leitura é em ordem alfabética: `BP_TestDispListener` antes de
`BP_TestDispSource`. O nó do evento nascia sem o pino Amount (a assinatura do dispatcher é da outra classe) e o fio,
salvo pelo índice do pino, era descartado. O IA_Move igual: o Action Value nascia `float` (o Input Action ainda não
estava carregado: `input_init` vinha depois do `anim_init`, que já lê Blueprints) e o fio para o Break Vector (vetor)
caía por tipo.
- Durante a varredura (`scan_all`) os pinos não são refeitos por asset; no fim, com tudo carregado, sim.
- Um asset que perdeu um fio durante a varredura (`BpAsset.relink`) é lido de novo no fim dela.
- `input_init` vem logo depois do `bp_init` (editor e jogo); sem o Input Action, o pino mantém o tipo que tinha.
- Medido: 0 falhas em 16 rodadas diretas, 10 de 10 no `ctest --repeat`, 3 de 3 depois dos testes 1–10 (antes ~1 em 4).

## Construction Script no editor (sugestão #64, 2026-11-04)
Como na Unreal: o Construction Script de um ator de Blueprint roda no viewport do editor quando o ator é posicionado,
movido ou alterado e quando a classe compila, e o que ele faz aparece enquanto o nível é montado. O que ele faz só é
**mostrado**, nunca autoral (`editor_construct.c`):
- Cada ator cuja classe tem um Construction Script com nós guarda o que é autoral (os componentes dele e dos seus
  componentes, menos nome, ligação de Blueprint, o transform derivado dos componentes e o estado que se mexe sozinho) e o
  que o script deixou. A cada quadro compara (memcmp de algumas centenas de bytes): diferença = edição do usuário. No
  transform da raiz a edição vira mudança do autoral (o script pode ter mexido nele); outro componente diferente vira o
  autoral como está. Então o autoral volta e o script roda de novo (`bp_construct_in_editor`).
- **Só o próprio ator** (`editor_cs_allows` na VM): transform, transforms/cores/luzes/visibilidade dos seus componentes.
  Nada de Spawn, Destroy, física, IA, widgets, input, timers, Open Level, céu, pós-processo, materiais dinâmicos,
  Print, dispatchers ou chamar outro ator. Uma instância própria na VM, esquecida em seguida (sem BeginPlay nem Tick).
- Antes de **salvar**, de dar **Play** e de guardar um passo do **Desfazer**, o autoral volta
  (`editor_construct_restore`): arquivo, Play e Desfazer veem o nível autoral; no quadro seguinte o script aparece de novo.
  Undo/Redo, outro nível e o fim do Play trocam o mundo: as tabelas recomeçam (`editor_construct_forget`).
- Um script mais lento que 2 ms espera o gizmo ser solto. Custo no Debug Viewer (F10): lista "Construction Scripts".
- **Medida**: o BP_TestDispSource (gira para yaw 33) roda em 0,042 ms (era 0,67 ms limpando a tabela de strings de 4 MB
  da VM a cada vez: agora só quando houve strings); parado, nada roda (0 ms). Teste `construction_editor` (`--cs-test`):
  girado no editor, movido mantém o giro e o movimento, autoral antes de salvar (yaw 0, movimento mantido), girado de
  novo, ocioso sem mudança.

## Instance Editable por ator (2026-11-08, plano de melhorias D1)
- `CBlueprint` cresceu no fim (níveis antigos leem zero): até 12 valores por ator (`CBpOverride`: hash FNV-1a do nome da
  variável, um registro ou um texto de 56 bytes). Salvo no `.rlevel` com o resto do componente; Ctrl+D e o snapshot
  do PIE copiam.
- `make_instance` aplica os valores sobre os padrões da classe antes do Construction Script e do BeginPlay: vale no
  Play, no jogo empacotado e no Construction Script do editor. `BpStats.overrides` conta.
- Details do ator, seção Blueprint: as variáveis Instance Editable da classe e dos pais Blueprint, com o editor de
  valor do próprio editor de Blueprint (`bp_value_edit`); valor mudado mostra a seta amarela (volta ao padrão).
- Tipos: bool, int, int64, byte, float, vector, rotator, color, enum, string, name, text. Fora: containers, structs,
  transform, actor, object, class (como "não editáveis por instância" por ora).
- Teste `blueprints`: o ator do teste muda Speed 1 → 7 e Label "classe" → "ator"; `game_blueprints` passa com o nível.

## Gameplay Tags (2026-11-08, plano de melhorias D5)
- `gameplay_tags.c`: componente `COMP_GAMEPLAY_TAGS` (16 nomes de 40 bytes), Details > Adicionar > Gameplay Tags (lista
  com + e x). Hierarquia por pontos: "Damage.Fire" tem "Damage"; comparação sem caixa (como o FName).
- Nós (categoria Gameplay Tags): Add / Remove Gameplay Tag, Has Matching Gameplay Tag, Has Gameplay Tag (Exact), Matches
  Tag. Target solto = o próprio ator; em Real/C#/Python/C viram métodos do ator (`AddGameplayTag("Team.Red")`).
- Testes `gameplay_tags` (`--tags-test`) e `blueprints` (BeginPlay põe State.Stunned, Has Matching(State) = true).
- (Feito em 2026-11-09: lista de tags do projeto, seletor, Get All Actors with Tag; ver "Lista de tags do projeto".)
- Falta: habilidades (R7, módulo 9).

## Format Text (2026-11-08, plano de melhorias B5b)
- Nó Format Text: `{0}`..`{3}` trocados pelas entradas (qualquer valor ligado vira texto), `{{`/`}}` escrevem a chave.
  Monta num buffer da pilha e grava na tabela de strings da VM (nada alocado). Teste `blueprints`: "Speed {0} {{ok}}" →
  "Speed 7 {ok}".

## Custom Event com entradas e em outro ator (2026-11-08, plano de melhorias B5c)
- A assinatura fica num grafo só de assinatura (`BPG_EVENT`, como o do dispatcher), com o nome do evento; criado no
  primeiro "+ Add" do Details do evento (Inputs: nome e tipo, até 6). `bp_event_signature` procura na classe e nos pais.
- O evento: `OP_GETARGS` na entrada lê as entradas (como o de interface). Call Custom Event: entradas encenadas com
  `OP_ARG`; sem Target Class, salta para o evento e volta (como antes); com Target Class, ganha o pino Target e
  `OP_CALLEV` roda o evento na instância do alvo (`run`, profundidade até 4).
- Teste `blueprints`: `Hit(Amount)` soma em LastHit; Hit(3) local + Hit(5) por Target → 8, uma chamada remota.

## Lista de tags do projeto (2026-11-09, sobra da Fase 1)
- `gameplay_tags.c`: a lista do projeto em `Config/DefaultGameplayTags.ini` (o formato da Unreal,
  `+GameplayTagList=(Tag="A.B",DevComment="")`), lida de novo quando outro projeto abre (`project_file`), ordenada,
  gravada a cada mudança; até 1024 tags. `gt_project_add/rename/remove/import_level`, `gt_find_actors` (exata ou como
  Has Matching; só os arquétipos com o componente). Renomear troca também as tags de baixo e as dos atores do nível.
- `gameplay_tags_ui.c` (editor): **Project Settings > Gameplay Tags** (árvore pelos pontos, pais implícitos em cinza,
  Adicionar, Adicionar Sub-tag, Renomear, x apaga com as de baixo, Trazer as Tags do Nível, busca) e o **seletor**
  (`gt_tag_picker`: combo com busca e árvore; um nome novo digitado entra na lista) no Details > Gameplay Tags e nos
  pinos `name` dos nós de tag (`bp_editor.c` lit_widget pelo tipo do nó).
- Nós **Get All Actors with Tag** (Utilities: exatamente o nome, as Actor Tags da Unreal são as Gameplay Tags aqui) e
  **Get All Actors with Gameplay Tag** (Gameplay Tags; Exact Match). Em scripts: `GetAllActorsWithGameplayTag("Enemy", false)`.
- Testes: `gameplay_tags` (lista: adicionar sem repetir, ordem, renomear em 2 tags e 2 atores, remover com as de baixo,
  trazer do nível; busca exata 1 e hierárquica 2) e `blueprints` (BeginPlay: With Gameplay Tag(State) 1, With Tag(State) 0).
- Artigo `gameplay-tags-list`.

## Debugger (2026-11-09, o primeiro item de produção)
Como o da Unreal. `bp_debug.h` (API), o runtime no fim de `bp_vm.c`, a fiber em `bp_fiber.c`, a UI em `bp_editor.c`
(seção "The Blueprint debugger") e em `script_ide.c`, o teste em `bp_debug_test.c`.
- **Mapa do código**: o compilador guarda por op o nó de origem (`BpAsset.op_src`: índice + 1 em `dbg_nodes`: dono,
  grafo, id, tipo, registrador da 1ª saída, linha do script) e o início de cada nó executável (`dbg_sites`: pc, nó,
  entrada exec). Um nó alcançado por dois caminhos tem uma cópia só: um site. O evento ganha site no pc dele (depois
  do do primeiro nó: um passo mostra o nó que vai rodar). `dbg_vars`: cada variável (da classe e dos pais) e o registrador.
- **Breakpoint = patch**: `OP_BREAK` (63) no lugar da op do site; a original fica numa tabela (`DbgPatch`). Sem
  breakpoint o código é o mesmo de antes: nada é checado por instrução. O `OP_BREAK` cai no `default` do switch (o
  switch das ops continua denso); `dbg_hit` decide (breakpoint ligado e da instância depurada, passo, rastro) e devolve a
  op original, executada no mesmo lugar (`goto redo`). Compilar de novo põe os patches de volta; o JIT é refeito quando
  o código muda (`jit_stale`, só numa execução de topo: nenhuma outra o segura); uma execução em andamento larga o
  nativo antigo (`a->jit_ok`). Não compila enquanto pausado.
- **A pausa**: com algum patch, `bp_tick`, `bp_begin_play`, `bp_key_event` e `bp_touch_update` rodam o corpo numa
  fiber própria (16 MB reservados). O breakpoint troca para a fiber principal (`bpf_suspend`): a pilha C da VM fica
  intacta (funções, eventos de outros atores, um Spawn no meio). O editor vê `bp_debug_paused()`: o bloco do Play chama
  só `bp_tick` (que retoma com um comando), física, animação e o resto esperam; F10/F11 deixam de ser Debug Viewer e
  tela cheia. Outra execução pedida de fora enquanto pausado não roda (`g_dbg_block`, um teste por execução). Parar o
  Play abandona a fiber (`bpf_abandon`). Uma execução fora do tick (clique de widget, evento online) que bate num
  breakpoint só avisa ("ran outside the game's tick").
- **Passos**: Step Over / Into / Out põem `OP_BREAK` de passo em todos os sites das classes compiladas até a próxima
  parada; a profundidade = execuções aninhadas × 64 + chamadas de função na pilha de retorno (a op antes do retorno é
  um Call Function / Call Custom Event). Num script o passo anda por linha (os nós da mesma linha seguem).
- **Valores**: o pino de saída lê o registrador (o nó de variável lê a variável; a entrada segue o fio; sem fio, o
  literal); structs por campo, arrays `[n] a, b, ...`, enums pelo nome. Variáveis da instância no painel.
- **Pilha de chamadas**: o nó parado, as funções na pilha de retorno e as execuções de fora (`V.fr`: a instância de cada
  profundidade e a op em que ela chamou: OP_CALLEV, OP_CALLIFACE, OP_DISPATCH, OP_SPAWN). Um nativo que roda eventos
  (Apply Damage...) aparece sem o nó (não grava nada por nativo).
- **Rastro**: com o editor do Blueprint aberto no Play, `OP_BREAK` de rastro nos sites da classe: cada nó executado guarda
  o tempo (anel de 2048); os fios de execução para ele brilham e somem em 0,7 s. Custa só com a janela aberta.
- **UI**: F9 / clique direito (Adicionar, Remover, Desligar Breakpoint), ponto vermelho (vazado: desligado), contorno
  amarelo e seta no nó parado, valor no tooltip do pino, "Observar Este Valor", barra (Depurar, Objeto de Depuração com
  as instâncias; selecionar um ator da classe no nível o escolhe; Continuar/Passos), janela Depurador de Blueprint
  (pilha clicável, observados, variáveis, breakpoints com acertos). Na pausa a janela do Blueprint vem para a frente
  centrada no nó e o mouse é solto (capturado de novo ao continuar). Scripts: ponto na margem do Editor de Código
  (clique ou F9), linha amarela parada. Breakpoints não vão para o asset (como os por usuário da Unreal).
- **Teste `bp_debugger`** (`--bp-debug-test`): classe com BeginPlay → Set Total(2+3) → Call AddTwice(Total) → Set Total
  → Print e Tick → Count + 1; script Real de 3 linhas. Para no Call (o Count fica 0 por 20 quadros), pino X e Total = 5,
  Step Into para na função (2 quadros na pilha), Step Over volta ao Set Total (Result 10) e ao Print (Total 10); a
  linha 6 do script (n 1) e o passo para a 7 (n 42); breakpoint no Tick (rastro com 0,000 s), removido, Continue.
  O nome da classe muda a cada rodada (o registro guarda o .rbp de uma rodada anterior lido no início) e a pasta é apagada.
- **Medidas** (RTX 3060, a máquina com as outras sessões rodando): o laço de 40000 voltas do teste com e sem breakpoint
  em outro evento da classe: 4,89/4,95 ms interpretado, 0,554/0,555 ms nativo. Contra o build de antes (o `scripts`,
  JIT measure, 20 rodadas alternadas, as duas ordens): medianas 4,52 × 4,53 ms interpretado, 0,52 × 0,54 ms nativo
  (dentro do ruído; a 1ª versão gravava a op em todo nativo e tinha o OP_BREAK num case: ~3% e foi tirada). Retomar
  da pausa: 0,008–0,012 ms. 66 acertos de OP_BREAK no teste (passos e rastro).
- Limites: um evento de nativo que não seja do tick não pausa; a pilha não mostra o nó de um nativo que chamou outro
  ator; variáveis locais de funções aparecem só pelos pinos; breakpoints não são salvos entre sessões.

## Arrastar para o grafo e menu em caixas (2026-11-10)
- Do My Blueprint: variáveis (já havia), macros (já havia), **funções** (`RE_BP_FUNC`: Call) e, do Components, um
  **componente** (`RE_BP_COMP`): o nó novo **Get Component** (`BPK_GET_COMPONENT`, puro: o componente de self pelo
  nome; também no menu, categoria Components).
- Menu do botão direito: cada categoria é uma caixa que abre e fecha (subcategorias "A|B" aninhadas); a busca abre
  todas com só o que bate; "Compatible" (puxando um fio) já abre aberta. Janela 400 x 460.

## Arrastar nós sem atraso e o delegate dos eventos (2026-11-10)
- Arrastar: o nó ia pelo `MouseDelta` enquanto era desenhado (um quadro atrás do cursor) e perdia os 2 px do limiar.
  Agora o arraste guarda as posições do começo e põe a seleção em início + (mouse − clique) **antes** da geometria.
- Delegate (o quadradinho vermelho da Unreal): `BpPin.delegate`, um pino exec que o compilador nunca segue como fluxo,
  sempre o último dos pinos (os fios salvos não mudam de lugar). Custom Event tem a saída **Delegate**; Bind / Unbind
  Event to e Set / Clear Timer by Event, a entrada **Event**. Ligado, o Bind guarda o nome do Custom Event
  (`BpBind.event`, operando d do `OP_DBIND`) e o Call do dispatcher roda esse evento com os argumentos; o timer usa o
  evento ligado em vez do escolhido no Details. Só liga delegate em delegate; fio vermelho. Teste `blueprints`: o
  ouvinte liga HandlePing pelo delegate (yaw 77).

## Pinos de asset e delegate no título (2026-11-10)
- `bp_asset_pin(kind)`: o asset / nome que o nó usa vira um pino Name (o último): Sequence, Sound, Sound Mix, Sound Class,
  System, Decal Material, Widget Name, Animation Name, Parameter Name, Key Name, Montage, Animation, Behavior Tree,
  Mapping Context. O literal é o `text` do nó (Details); ligado, o compilador põe o valor no último slot de parâmetro
  (`OP_NPARAM`) e o operando do nome vira `BP_NAME_STAGED` (`name_at` no VM); os nós com o nome num registrador (sons,
  partículas, decal) usam o registrador do pino. Corrigido junto: Play Montage, Play Animation, Run Behavior Tree e Set
  Blackboard Value caíam no caminho genérico e liam o primeiro texto da classe.
- O delegate (quadradinho vermelho) do Custom Event fica no título, à direita.
- Os menus do grafo (botão direito, pino, nó, soltar variável) usam a fonte do editor: o mesmo tamanho em qualquer zoom.

## O Mesh do Character sumido (2026-11-10, relatório do usuário)
- **Causa:** cada nó de componente do Blueprint guardava o componente em `data[96]`; o `CSkeletalMesh` cresceu para 340
  bytes (os 64 pesos de morph target) e o spawn (`bp_components.c`) só copiava componentes que cabiam: o Mesh do
  Character nascia vazio (sem Mannequin no nível nem no Play com o Blueprint). O `CBlueprint` (Blueprint Component)
  também não cabia e perdia a classe.
- **Correção:** `BP_SCS_DATA` = 352 com `_Static_assert` por tipo de componente; um componente maior que o nó (o
  `CBlueprint` com os valores por ator) guarda só o começo e o resto fica nos padrões. O `.rbp` grava o hex até o último
  byte não zero; arquivos antigos (96 bytes) abrem, e um Skeletal Mesh salvo sem os padrões ganha play rate 1 e loop.
- **Viewport do Blueprint:** desenhava só Static Mesh; agora os Skeletal Mesh aparecem na pose de referência (partes
  rígidas e skins dobradas na GPU), no enquadramento e nas miniaturas.
- Teste `bp_viewport_character` (projeto do `project_third_person`): a prévia desenha triângulos (antes 0).

## Variáveis como na Unreal (2026-11-10, pedido do usuário)
- **Lista Meu Blueprint**: a pílula colorida do tipo (clique: o seletor de tipo e contêiner ali, os nós Get/Set
  retipados) e o olho do **Instance Editable**; o valor na própria linha só quando a lista é larga.
- **Seletor de tipos organizado** (`type_combo_ex`): busca, Básicos, Estruturas, Enumerações, **Referências de Asset**
  (21 classes: Level Sequence, Animation Sequence, Anim Montage, Sound, Sound Mix/Class, Material, Texture, Static/Skeletal
  Mesh, Particle System, Behavior Tree, Data Asset, Dialogue, Quest, Input Action/Mapping Context, Level, Vehicle,
  Physics Asset, IK Rig), Referências de Objeto e de Classe.
- **Referência de asset** = `BPT_NAME` com a classe em `ref` (o .rbp não muda de formato; a VM não muda): o valor é o
  nome do asset, o padrão escolhido entre os assets do projeto com aquela extensão (`asset_ref_pick`), e liga nos pinos
  de asset dos nós (Name).
- **Expose on Spawn** (`BpVar.expose_spawn`, JSON `expose_on_spawn`): o Spawn Actor from Class (e o Async) ganha um
  pino por variável exposta da classe escolhida (`build_pins`, depois de todos: os fios salvos ficam no lugar; a troca
  da Class refaz os pinos). A VM empilha os valores (`OP_SPAWNARG`, pelo hash do nome; textos copiados) e a instância
  nova da classe os aplica depois dos padrões e dos valores por ator, antes do Construction Script e do BeginPlay.
  Só valores simples (`bp_var_overridable`).
- Teste: `blueprints` (`bp_test.c`): BP_TestSpawner cria BP_TestSpawned com Size 3 no pino; o BeginPlay dele lê Size e
  se escala: 3,00.
- **Bug achado no kit (Size 0 às vezes):** ao ler o `.rbp`, o Spawn Actor era criado antes do literal da Class, o pino
  exposto não existia e o valor salvo dele era descartado; um refresh depois o recriava com 0 (só quando o Blueprint era
  relido do disco: no kit, 3 de 3; sozinho, nunca). Agora a leitura refaz os pinos expostos depois da Class e aplica os
  valores de novo (pinos provisórios se a classe ainda não carregou), e o pino exposto guarda o valor pelo nome mesmo
  quando o tipo chega depois. Kit: `blueprints` passa.

## Timeline (2026-11-16, "o node de timeline que a Unreal tem")
- **Dados** (`bp_timeline.c/.h`): a classe tem as Timelines (`BpAsset.tl`, alocado só com a primeira), no `.rbp` em
  `"timelines"`. Cada uma tem nome, Length, Use Last Keyframe, AutoPlay, Loop, Play Rate e até 6 trilhas (Float, Vector,
  Linear Color, Event) com até 64 chaves (Auto = Hermite com tangentes de Catmull-Rom, Linear, Constant). Uma filha vê as
  do pai.
- **Nó** `BPK_TIMELINE` (text = a timeline). Entradas: Play, Play from Start, Stop, Reverse, Reverse from End, Set New
  Time (New Time). Saídas: Update, Finished, Direction e um pino por trilha (Event = exec). Os pinos das trilhas vêm de
  `build_pins`, e o nó se refaz quando o editor muda a timeline.
- **Compilação** (`bp_vm.c`): cada entrada vira `OP_TIMELINE slot, ordem, New Time` (então são até 8 entradas exec por nó:
  `exec_pc` de 4 para 8). No Event Graph, a Timeline emite os blocos Update, Finished e um por Event track, e guarda os
  registradores de Direction e das trilhas em `BpAsset.tlc[8]`.
- **Execução**: cada instância tem `TlState {t, dir, playing}` por Timeline, no arena dela. O `bp_tick` avança o tempo,
  escreve as trilhas direto nos registradores das saídas, roda Update, os Event tracks cruzados (com volta no Loop) e
  Finished. AutoPlay começa na criação da instância. Nada é alocado.
- **Editor** (`bp_timeline_ui.c`): janela própria aberta por dois cliques no nó (ou ao colocar um novo). Barra com nome,
  Length, Use Last Keyframe, AutoPlay, Loop e Play Rate. Lista de trilhas com +Float/+Vector/+Color/+Event e o botão
  direito para renomear ou apagar. Curva com grade de segundos e a marca do fim. Shift+clique cria chave, arrastar move,
  Delete apaga, botão direito escolhe a interpolação; roda faz zoom (Ctrl nos valores) e o botão direito move a vista.
  Embaixo, os campos Time e Value da chave escolhida.
- **Medidas**: 1 M leituras de uma trilha Vector de 64 chaves em 46 ms (46 ns cada, busca binária).
- **Testes**: no `blueprints`, `bp_timeline_selftest` (ordem, Auto, Linear, Constant, pontas, velocidade) e
  BP_TestTimeline tocado de verdade: Value 2,000, o Event em 0,5 s uma vez, Finished.
