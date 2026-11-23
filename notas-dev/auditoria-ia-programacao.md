# Auditoria de IA e de programação (Q24, lote 8 do plano 0.43, 18/11/2026)

A Real Engine comparada com a Unreal 5.8 (e com o que estúdios usam em 2026) em **IA de jogo** e em **programação**. O
resultado vira itens do plano 0.44 (seção 3) e três correções pequenas feitas aqui (seção 4). Tamanhos: **P** (< 1
dia), **M** (1-3 dias), **G** (1-2 semanas), **GG** (mais). A numeração `Q24.n` é provisória: a sessão principal renumera
ao juntar com o Q23 (ferramentas de artista).

Lido: `src/ai.c`, `ai.h`, `ai_editor.c`, `navmesh*.c`, `crowd.c`, `blueprint.c/h`, `bp_vm.c`, `bp_debug.h`, `bp_jit.c`,
`script_*.c`, `plugin_api.h`, `plugins.c`, `debug_viewer.c` e as notas `ia.md`, `multidao.md`, `blueprints.md`,
`scripts.md`, `testes.md`, `editor.md` (Plugins), `debug-viewer.md`.

## 1. IA

| Área | Hoje (onde) | Unreal 5.8 / estúdios 2026 | Falta | Prioridade · tamanho |
|---|---|---|---|---|
| **Behavior Tree** | `.rbt` (`ai.c`, `ai_editor.c`, ia.md): Selector, Sequence; 11 tarefas (Move To, Wait, Rotate, Find Random, Set/Clear Key, Run Event, Play Montage, Predict, Surround); decoradores Blackboard (com observer aborts), Cooldown, Loop, Time Limit, Force Success; serviços Default Focus e Set Movement Speed; até 64 nós | BT com Simple Parallel, Run Behavior (subárvore) e Run Behavior Dynamic, **tarefas/decoradores/serviços em Blueprint** (BTTask_BlueprintBase), Is At Location, Does Path Exist, Compare BB Entries, Conditional Loop, Gameplay Tag, Run EQS Query; debugger com o nó ativo aceso e breakpoints | Simple Parallel, subárvore, nós em Blueprint/script, decoradores que faltam, nó ativo aceso no editor ao vivo, limite fixo de 64 nós | **alta · M** |
| **Blackboard** | chaves bool, float, vector, actor (24 por asset) | + int, enum, name, string, rotator, object, class; Instance Synced; herdar Blackboard | tipos e herança | **alta · P** (junto do item de nós de IA) |
| **State Tree** | não existe (o Real Brain cobre a decisão por utilidade, ia.md) | State Tree é o padrão novo da Unreal para IA e lógica de jogo (estados hierárquicos, tarefas, condições, transições, avaliadores, utilidade na seleção; roda no Mass) | asset, editor, depuração, roda em ator e na multidão | **alta · G** |
| **EQS** | não existe; consultas fixas: Find Random Location, Find Surround Spot, `ai_nav_random_point` | Environment Query System: geradores (grade, anel, pontos do caminho, atores da classe), testes (distância, produto escalar, traço/visibilidade, caminho, tag), pontuação e filtro, fatiado no tempo, depuração visual | o sistema inteiro: cobertura, flanco, fuga, posição de tiro | **alta · M/G** |
| **Smart Objects** | não existe para a IA; Interactable/Interactor é do jogador (interacao.md) | Smart Objects: lugares com vagas reservadas (banco, balcão, porta), comportamento por definição, busca por tags | componente, reserva de vaga, busca, comportamento (montage, BT, evento); NPCs de cidade | **média · M** |
| **Mass AI / multidões** | Crowd Spawner (multidao.md): milhares de agentes SoA, campo de fluxo, Hive/Individual, desenho na GPU com LOD | Mass Entity + Mass AI: State Tree por agente, LOD de IA (simulação completa perto, barata longe), Smart Objects, ZoneGraph (faixas de pedestres e trânsito), representação por LOD | comportamento por State Tree na multidão, troca agente ↔ multidão sem pop, faixas | **média · G** |
| **Percepção** | visão (raio, cone, linha de visada), audição (Report Noise Event, passos), memória, times, previsão e esquadrão; **sentido de dano novo (Q24)** | AI Perception: Sight, Hearing, **Damage**, Team, Touch, Prediction; **Stimuli Source** (qualquer ator percebido), afiliação amigo/neutro/hostil, eventos OnTargetPerceptionUpdated/Forgotten, Get Perceived Actors | Stimuli Source (hoje só o jogador e agentes de outro time), Team/Touch, eventos e consultas no Blueprint, afiliação | **alta · M** |
| **NavMesh** | Recast/Detour próprio (ia.md): tiles em segundo plano, Nav Links de andar/pular, Nav Modifier (Obstáculo, Água Rasa, Nula), Static/Stationary refeitos jogando, 120 m em volta dos agentes em níveis grandes; grade 2.5D de reserva (mundos procedurais) | Navegação dinâmica (Dynamic Obstacle e modificadores que andam), **Smart Links** com evento (porta, escada, elevador), vários tipos de agente, Navigation Invokers, dados por célula do World Partition | obstáculos que se movem (Movable hoje não conta), Smart Links, tipos de agente, NavMesh nos mundos procedurais, streaming por célula | **alta · M/G** |
| **Desvio entre agentes** | **novo (Q24)**: desvio local tipo RVO em todo agente (hash por quadro, ninguém atravessa ninguém); a multidão tem separação própria | RVO Avoidance e Detour Crowd (ORCA, filas em portas, prioridades) | ORCA por velocidade, prioridade, filas em passagens estreitas | **média · M** |
| **Depuração** | tecla `'` (caminho, cone, o que faz), Debug Viewer > AI, nota ao vivo do Real Brain, Trace (Insights) | **Gameplay Debugger** (categorias: BT, Perception, EQS, Nav, Abilities) e **Visual Logger** (gravação por agente com formas e logs, linha do tempo, salvar) | categorias por sistema, Visual Logger, BT ao vivo | **alta · M** |
| **Nós de IA no Blueprint** | Get/Set Blackboard (4 tipos), AI Move To (sem saídas), Run Behavior Tree, Report Noise Event, nós de navegação | AI Move To latente com On Success/On Fail e o resultado, Stop Movement, Set/Clear Focus, Get AI Controller, Use Blackboard, eventos de percepção, Move To Location/Actor | latente com resultado, foco, parar, eventos | **alta · P/M** |
| **Aprendizado de máquina** | não existe | Learning Agents (imitação e reforço), NPCs com diálogo por LLM em estúdios | treino fora do jogo, inferência leve | **baixa · GG** |

## 2. Programação

| Área | Hoje (onde) | Unreal 5.8 / estúdios 2026 | Falta | Prioridade · tamanho |
|---|---|---|---|---|
| **Blueprints** | classes, structs, enums, interfaces, Data Assets, **Macro Library** (`BPA_MACROLIB`), Event Dispatchers, Timeline, Construction Script, Expose on Spawn, Instance Editable (blueprints.md) | + **Blueprint Function Library**, Find in Blueprints (busca global), **Blueprint Diff**, Async Actions próprias | Function Library, busca global, Diff | **alta · P/M** |
| **Depurador** | breakpoints por patch (`OP_BREAK`), passos, valores, pilha, rastro, observar; vale nos scripts (blueprints.md "Debugger") | + breakpoints condicionais, contagem de acertos, locais de função, breakpoints salvos, depurar eventos fora do tick, depuração externa (Rider/VS) | condicional, contagem, locais, salvos, eventos fora do tick, DAP (VS Code) | **média · M** |
| **Código C/C#/Python/Real** | 4 linguagens compiladas para o grafo e a VM, IDE com completar, erros ao digitar, F12 (scripts.md); **do ... while novo (Q24)** | C++ com Live Coding; Verse (UEFN); estúdios: C# (Unity), Lua/Luau, Python no editor | várias classes por arquivo, `continue` no do ... while, ler variáveis de outro ator por cast (`(other as Enemy).hp`), limite de 64 variáveis, LSP para editores externos | **média · M** |
| **Hot reload** | scripts recompilam fora do Play (0,5 s); Blueprint compilado no Play só vale no próximo Play; plugin só na próxima abertura | Live Coding (troca o código jogando), recompilar Blueprint no PIE refaz as instâncias | trocar instâncias vivas no Play (variáveis por nome), recarregar a DLL do plugin | **média · M** |
| **Profiler de scripts** | Debug Viewer > Blueprints (ms e ops por instância), Trace como o Insights | Insights com eventos por função de Blueprint, Blueprint Profiler (por nó, calor) | por função e por nó, calor no grafo e na margem do Editor de Código | **média · M** |
| **Testes automatizados do jogo** | só a suíte da engine (ctest, testes.md); o usuário não escreve testes | Automation System: **Functional Test** (ator no nível), Automation Spec, Session Frontend, linha de comando com relatório | ator Functional Test, specs em script, janela de testes, `--run-automation` com relatório JUnit | **alta · M** |
| **Editor Utility Widgets** | Utilitários do Editor: cada Custom Event vira um botão (scripts.md) | Editor Utility Widgets (UMG numa aba do editor), Editor Utility Blueprints com APIs de assets e atores | EUW pelo Designer do UMG, nós de editor para assets | **média · M** |
| **Plugins e API de C** | DLL + `plugin.json`, nós (8 entradas, 4 saídas), tick, menu, `log`, `status`, `actor_count`, `in_editor` (`plugin_api.h`, editor.md) | módulos C++ com acesso a tudo, conteúdo no plugin, Fab | API v2: atores, componentes, propriedades por nome (tabela do Q16), spawn, eventos, assets; recarga sem reabrir; conteúdo no plugin | **média · G** |
| **Documentação gerada** | Referência da API (janela), Assistente com artigos | site da API gerado do código | `--api-doc` (Markdown/HTML de todos os nós com exemplos nas 4 linguagens) publicado com a release | **baixa · P/M** |
| **Compilação nativa** | JIT x64 do bytecode (`bp_jit.c`): aritmética, saltos, vetores; o resto volta ao interpretador | nativização saiu da Unreal 5; estúdios usam VMs com JIT (Luau, .NET) ou AOT | chamadas, arrays e strings no JIT; **JIT ARM64** (Android, Quest do Q25: hoje interpretado); AOT "Blueprints → C" no pacote (etapa 3 do RealEngine.md) | **média · G** |

## 3. Plano 0.44 (itens Q24.n; numeração provisória)

Regras de sempre: medida antes/depois em ms, teste no ctest com área em `tools/suite_areas.cmake`, nota em `notas-dev/`,
artigo no Assistente, textos pt/en/es. Ordem sugerida por lote: (A) Q24.1, Q24.8, Q24.11; (B) Q24.2, Q24.4, Q24.7;
(C) Q24.5, Q24.12+Q24.15, Q24.16; depois o resto.

### IA
- **Q24.1. State Tree** (alta, G) — asset `.rstatetree`: estados hierárquicos, tarefas (as do BT e as do Brain), condições,
  transições (ao terminar, por evento, por tempo, por utilidade), avaliadores globais, parâmetros; roda num ator (componente
  IA, modo State Tree) e é o cérebro da multidão no Q24.9. Editor no layout da Unreal com o estado ativo aceso ao vivo.
  Pronto: teste `statetree` (guarda: patrulha → alerta → combate → busca, igual ao BT_Chase). Medida: < 0,002 ms por agente
  por quadro com 100 agentes. Área: `ia`.
- **Q24.2. EQS** (alta, M/G) — asset `.rquery`: geradores (grade, anel, pontos no NavMesh, atores de classe/tag), testes
  (distância, ângulo, linha de visada contra o alvo, comprimento do caminho, tag), pontuação e filtro; rodado nos workers
  e fatiado no tempo; tarefa de BT **Run EQS Query**, entrada do Brain e nó de Blueprint; pontos coloridos na tecla `'`.
  Pronto: teste `eqs` (cobertura fora da visão do jogador escolhida; fuga para longe de 3 inimigos). Medida: consulta de
  200 pontos < 0,1 ms num worker, 0 ms na thread do jogo. Área: `ia`.
- **Q24.3. Smart Objects** (média, M) — componente Smart Object (vagas com pose e direção, tags, reserva), definição com o
  comportamento (montage, BT, evento), busca por tag no raio (Find Smart Object), Claim/Use/Release na BT e no Blueprint;
  liga no Interactable. Pronto: teste `smart_objects` (6 NPCs dividem 4 bancos sem dois na mesma vaga). Área: `ia`.
- **Q24.4. Percepção completa** (alta, M) — componente **AI Perception Stimuli Source** (qualquer ator visto/ouvido, por
  sentido), sentidos Team e Touch, afiliação (amigo/neutro/hostil por par de times), eventos **On Target Perception
  Updated / Forgotten** e **Get Perceived Actors**, idade máxima por sentido. Pronto: teste `perception` (um ator
  qualquer com Stimuli é visto; amigo não vira alvo; o evento chega no Blueprint). Medida: percepção de 100 agentes ×
  50 estímulos < 0,1 ms por quadro (escalonada). Área: `ia`.
- **Q24.5. NavMesh dinâmica e mundos grandes** (alta, M/G) — obstáculos Movable (Dynamic Obstacle: área recortada nos tiles
  que cobre, refeita em lotes), **Smart Links** (Nav Link com evento: porta, escada, elevador), tipos de agente (pequeno,
  humano, grande: um NavMesh cada), NavMesh nos Mundos Procedurais e por célula do World Partition. Pronto: teste
  `nav_dynamic` (caixa empurrada fecha o corredor, a IA contorna; porta abre pelo evento do link). Medida: tile refeito
  < 1 ms no worker, 0 travadas na thread do jogo. Área: `ia`.
- **Q24.6. Desvio por velocidade (ORCA)** (média, M) — o desvio do Q24 (seção 4) vira ORCA com prioridade, filas em
  passagens estreitas e a multidão no mesmo hash. Pronto: teste `avoid_doorway` (12 agentes passam por uma porta sem se
  atravessar). Medida: 1000 agentes < 0,3 ms. Área: `ia`, `multidao`.
- **Q24.7. Depuração de IA** (alta, M) — Gameplay Debugger em categorias (BT/State Tree, Brain, Percepção, EQS, NavMesh,
  Habilidades) escolhidas no teclado numérico como na Unreal; **Visual Logger** (anel por agente: textos, setas,
  caixas, caminhos; janela com linha do tempo, rolar e salvar `.rvlog`); BT ao vivo (nó ativo aceso, decoradores que
  passaram) e breakpoint num nó da BT. Pronto: teste `vlog` (gravado, salvo, aberto, 1 entrada por decisão). Medida: 0 ms
  com a gravação desligada, < 0,05 ms ligada com 20 agentes. Área: `ia`, `debug-viewer`.
- **Q24.8. Nós e chaves de IA** (alta, P/M) — AI Move To latente (On Success, On Fail, resultado), Stop Movement, Set/Clear
  Focus, Clear Blackboard Value, Get/Set Blackboard as Int/Name/Rotator/Enum; chaves int, enum, name, rotator, object,
  class; BT: Simple Parallel, Run Behavior (subárvore), decoradores Is At Location, Does Path Exist, Compare BB Entries,
  Gameplay Tag; **tarefas, decoradores e serviços em Blueprint/script** (classes filhas de BTTask/BTDecorator/BTService);
  limite de 64 nós vira dinâmico. Pronto: teste `ai` estendido (subárvore, paralelo, tarefa em Blueprint terminando com
  Finish Execute). Área: `ia`, `blueprints`.
- **Q24.9. Mass AI** (média, G) — a multidão roda State Tree (Q24.1) por agente em SoA, LOD de IA (agente completo perto,
  multidão longe, troca sem pop), Smart Objects para a multidão, faixas (pedestres e trânsito). Pronto: cena `city`
  (2000 pedestres em faixas usando bancos). Medida: 2000 agentes < 1 ms de CPU. Área: `multidao`.
- **Q24.10. Learning Agents** (baixa, GG) — imitação/reforço treinados fora do jogo, inferência pequena na CPU. Só depois
  de tudo acima.

### Programação
- **Q24.11. Testes automatizados do jogo** (alta, M) — ator **Functional Test** (Prepare Test, Start Test, Finish Test com
  Passou/Falhou/Mensagem, tempo limite), **Automation Spec** nos scripts (`describe`/`it`/`expect` em Real, C#, Python e C),
  janela **Ferramentas > Testes Automatizados** (lista por nível/classe, rodar, resultado, ir para o nó que falhou), linha
  de comando `--run-automation <filtro>` (janela escondida, relatório JUnit XML) e passo opcional no Empacotar. Pronto:
  teste `functional_tests` (1 passa, 1 falha de propósito, 1 estoura o tempo: o relatório diz os três). Área: `blueprints`,
  `scripts`.
- **Q24.12. Blueprint Function Library** (alta, P/M) — asset `.rbfl`: funções sem self (puras ou não), chamáveis de
  qualquer Blueprint e script (`Lib.Funcao()`), categoria, descrição; + **Find in Blueprints** (Ctrl+Shift+F: busca global
  em nós, variáveis e comentários de todos os assets). Pronto: teste `blueprints` com uma biblioteca usada por 2 classes e
  um script. Área: `blueprints`.
- **Q24.13. Editor Utility Widgets** (média, M) — Widget Blueprint de editor feito no Designer do UMG, aberto numa aba
  acoplável (Run Editor Utility Widget); nós de editor para assets (listar, selecionar, renomear, duplicar, importar) e
  atores (spawn, propriedades por nome do Q16). Pronto: teste `euw` (o painel renomeia os assets selecionados com Ctrl+Z).
  Área: `scripts`, `umg`.
- **Q24.14. Hot reload no Play** (média, M) — recompilar um Blueprint ou script jogando troca as instâncias vivas (variáveis
  pelo nome, valores mantidos, timers e Delay recomeçados); plugin recarregado sem reabrir (DLL copiada para uma sombra).
  Pronto: teste `hot_reload` (mudar a velocidade no script jogando muda o ator sem parar). Medida: troca < 2 ms para 100
  instâncias. Área: `scripts`.
- **Q24.15. Depurador completo** (média, M) — breakpoints condicionais e por contagem, observar expressões, locais de
  função, breakpoints salvos por usuário, pausa em eventos fora do tick; DAP para o VS Code (opcional). Pronto: teste
  `bp_debugger` estendido. Medida: sem breakpoint, o mesmo custo de hoje (patch). Área: `blueprints`.
- **Q24.16. Profiler de scripts** (média, M) — tempo e ops por função e por nó (contadores por site só com o Debug Viewer
  ou o Trace pedindo), calor no grafo e na margem do Editor de Código, eventos por função no Trace. Pronto: teste
  `script_profile` (o laço quente aparece no topo). Medida: desligado 0%, ligado < 5% na VM. Área: `debug-viewer`, `scripts`.
- **Q24.17. Compilação nativa** (média, G) — JIT para chamadas de função, arrays e strings; **JIT ARM64** (Android e o Quest
  do Q25); AOT "Blueprints → C" no Empacotar (etapa 3). Pronto: teste `scripts` (JIT ≥ 8× no laço, igual em ARM64 no
  `mobile_selftest`). Área: `scripts`, `android`.
- **Q24.18. API de C v2 e plugins** (média, G) — atores, componentes, propriedades por nome, spawn, eventos, assets e
  conteúdo dentro do plugin; versão da API no `plugin.json`; recarga a quente. Pronto: teste `plugins` com um plugin que
  cria atores e lê propriedades. Área: `editor`.
- **Q24.19. Documentação gerada** (baixa, P/M) — `--api-doc <pasta>`: Markdown e HTML de todos os nós (assinatura,
  descrição, exemplos nas 4 linguagens), índice por categoria, publicado com a release. Pronto: teste `api_doc` (todo nó
  com página). Área: `scripts`.
- **Q24.20. Blueprint Diff** (baixa, M) — comparar com a versão do Git (nós novos, apagados, mudados; lado a lado). Área:
  `blueprints`.
- **Q24.21. Sobras dos scripts** (média, P/M) — `continue` no do ... while, várias classes por arquivo, cast para ler
  variáveis de outro ator, mais de 64 variáveis, LSP para editores externos. Área: `scripts`.

## 4. Correções pequenas feitas aqui (Q24)

### Sentido de dano (AISense_Damage)
`ai_report_damage` (`ai.c`), chamado pelo `health_apply_damage` (`health.c`) logo depois do HUD: um agente atingido sabe
na hora quem bateu, de qualquer lado, sem cone e sem tempo de reação. O instigador vira o alvo (`TargetActor`), o lugar
dele o último conhecido (seguido 1,5 s pela atualização) e um barulho (`HeardNoise`/`NoiseLocation` por 4 s); parado, o
pawn vira para ele 2 s enquanto não o vê. Fogo amigo (mesmo time) e agentes de time 0 não contam. Antes: um inimigo
atingido pelas costas não reagia (só via o atirador por acaso).

### Desvio local entre agentes (tipo RVO)
Os Characters não colidem entre si (as caixas do jogo não incluem Characters), e dois agentes andando um contra o outro
se atravessavam. Agora, a cada quadro, um hash de 1024 células de 3 m com os agentes e o pawn do jogador (arrays
estáticos, sem alocar); quem vai encontrar outro nos próximos 1,2 s se desvia (para longe do ponto do encontro; de frente,
cada um para a sua direita) e quem encosta se afasta. O alvo e o ator do Move To não contam. `REALENGINE_AI_AVOID=0`
desliga (para comparar). AiStats ganhou `avoiding` (agentes desviando no quadro) e `damage_sensed`.

### do ... while em C# e C
Antes: erro "do ... while isn't supported". Agora vira `while (true) { corpo; if (!(cond)) break; }` no parser
(`script_grammar.inc`); um `continue` no corpo (que pularia a condição) dá erro claro. O C# do teste `scripts` usa um.

### Medidas (i7-11700F, RTX 3060, 1440p, Release)
GPU dividida com outra sessão (havia outro `realengine.exe` aberto): o fps das cenas oscilou 160-250 nos dois lados, e
as falhas "< 200 fps" se alternaram entre desvio ligado e desligado; o custo é comparado pelo tempo da IA (`ai_stats`),
em pares alternados com o desvio desligado (`REALENGINE_AI_AVOID=0`, a base).

| O quê | Antes / desligado | Depois / ligado |
|---|---|---|
| `ai_senses`: dois Characters de frente, menor distância entre eles (raios 34 + 34) | 1 cm e 3 cm (se atravessam) | 87 cm nas 3 rodadas (passam um ao lado do outro), chegam a 31 cm do alvo |
| `ai_senses`: tempo da IA por quadro (5 agentes) | 0,0171 / 0,0134 ms | 0,0144 / 0,0147 / 0,0137 ms (ruído) |
| `ai_senses`: Victim atingida pelas costas a 10 m | não sabia de nada (só via o atirador se virasse por acaso) | sabe na hora, vira e vê o Shooter em 0,15-0,19 s; o Control (não atingido) não sabe |
| `ai_squad` (4 caçadores + alvo): resultado | 4 de 4 sabem, 2 flanqueiam, 4 miram à frente, 365 cm, vão 117° | 4 de 4, 2, 4, 366-394 cm, vão 110-117° (igual) |
| `ai_squad`: tempo da IA por quadro | 0,013 / 0,016 ms | 0,016 / 0,027 ms (+0,003 a 0,011 ms: o hash e a varredura dos vizinhos) |
| `scripts`: C# com do ... while | erro de compilação | "n=5 sum=45" (as 4 linguagens iguais); JIT 8,7× no laço |
| Assistente | 156 perguntas | 158 de 158 (2 perguntas novas do artigo `ai-damage-avoidance`) |

O sentido de dano custa só no quadro do Apply Damage (uma busca do agente, sem varredura); o desvio, um hash por quadro
com os agentes (4 KB de cabeças, 24 B por agente, estático) e a vizinhança 3 × 3 de cada um que anda.

### Suíte (18/11, 2º monitor, GPU dividida)
`tools/suite.ps1 -Visivel -Areas ia,scripts,saude-dano,assistente`: 53 de 82. Outras sessões rodavam o editor e testes
ao mesmo tempo (`hud-modular`, `eager-bassi`, `merge-lote7`), e 27 falhas são só "fps < limite" (as cenas a 90-198 fps).
Os resultados lógicos passaram: `ai_senses` PASS, `ai_squad` e `ai_squad_mm` com o esquadrão igual, `ai` 3 agentes,
`scripts` (4 linguagens iguais), `health`, `assistant` 158/158. As outras duas: `world_partition` ("a hitch", instável
na base também) e `async` (6 quadros em 10 s, um quadro de 9,97 s com a CPU carregada; não passa pela IA nem pelos
scripts; na base, às 18:44, passou): repetir com a máquina livre no merge.

## 5. Merge (para a sessão principal)
- **Arquivos**: `src/ai.c`, `src/ai.h` (AiStats cresceu: só interno), `src/health.c` (uma linha + include),
  `src/ai_senses_test.c/.h` (novos), `src/editor.c` (include e a cena, ao lado das linhas do Q21), `src/main.c` (include,
  tick e check ao lado do Q15/Q21), `src/script_grammar.inc`, `src/script_test.c`, `CMakeLists.txt` (fonte depois do
  `farview_test.c`, teste `ai_senses` depois do `ai_squad`), `tools/suite_areas.cmake` (`ai_senses` na área `ia`),
  `assistant/articles/ai-damage-avoidance.md` (novo), `csharp-guide.md` e `c-guide.md` (do ... while), `assistant/coverage.txt`
  (linha `ia` e a linha nova `auditoria-ia-programacao`), `src/assistant_test.c` (2 perguntas), `notas-dev/ia.md`,
  `notas-dev/scripts.md`, esta nota e o índice do `RealEngine.md`.
- **Formatos e bits**: nenhum. Nenhum componente mudou (CAIAgent igual), nenhum nó novo de Blueprint, nenhum opcode.
- **Conflitos prováveis**: as listas do `CMakeLists.txt` e do `suite_areas.cmake`, os includes e as linhas de cena/tick/check
  do `main.c` e do `editor.c` (o Q23 e o Q25 também põem testes ali), as linhas do `coverage.txt`, as perguntas do `assistant_test.c` e o índice do
  `RealEngine.md`. Nada no `i18n_table.c` nem nos tips.
- **Atenção**: o desvio muda o caminho de todo agente que anda perto de outro (o `ai_squad` e o `ai` foram rodados com ele);
  um teste do Q23/Q25 com IA que meça posições exatas pode precisar de `REALENGINE_AI_AVOID=0` ou de tolerância.
