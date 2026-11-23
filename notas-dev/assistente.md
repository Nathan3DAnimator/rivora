# Assistente (sem IA)

Pedido de 2026-11-07: um assistente dentro do editor, onde o usuário pergunta e recebe a resposta.
- **Sem IA:** nada de modelo de linguagem.
- **Leve:** a base de conhecimento não vem dentro do programa. Ela é buscada num repositório público do GitHub ou
  baixada pelo usuário para usar offline.

Abre em **Ajuda > Assistente** ou com **F1** (no Editor de Código, F1 continua sendo a referência da API).

## Para quem é
A engine inteira, para qualquer nível: de "nunca usei uma engine" (conceitos, glossário, as partes do editor, navegar
no 3D, colocar e mover coisas) a Blueprints (funções, comunicação entre Blueprints, Structures, Construction Script,
depuração), às receitas de mecânicas e aos sistemas complexos para profissionais (orçamento de desempenho, TSR, GI,
sombras, malhas pesadas, ECS, JIT, a API de tarefas, replicação, IA, animação, áudio, linha de comando).
- **Formal, informal, gíria e abreviações:** as palavras de enchimento ("mano", "tipo", "vc", "pq", "lol", "bro") são
  ignoradas; os sinônimos cobrem "bagulho/treco/coisa" = objeto, "travando/lag/fps lá embaixo" = desempenho, "bp" =
  blueprint...
- **Quase em código:** nomes de funções (`IsKeyDown`, `play_sound_2d`), trechos (`def on_begin_overlap(self, other)`,
  `for i in 0..10`) e mensagens de erro coladas encontram a função, o conceito ou a explicação.
- **Código do jeito pedido:** cada receita vem em variantes: **Blueprint** (a cadeia de nós) e o código em Real, C#,
  Python e C (as que existirem). A resposta mostra uma só:
  - a que a pergunta pede ("em python", "com blueprint", "c#", ou o código colado: `def`, `self.`, `public`, `#include`);
  - senão, a escolhida na barra **Mostrar como**, guardada no `editor.ini` (`assistant_variant`; o padrão é Blueprint).

## As fontes
| Fonte | Onde está | Peso no executável |
|---|---|---|
| **Artigos** (conceitos, como fazer, receitas em variantes, erros explicados, sistemas a fundo) | `assistant/articles/*.md` neste repositório. `tools/assistant_pack.py` gera um `index.json` (hoje 117 artigos, 153 KB, 95 grupos de sinônimos), publicado no repositório público `realengine-assistant` | nenhum |
| **Funções da engine** (346, com assinatura e descrição) | a lista da API dos scripts (`script_api_*`) | já existia |
| **Descrição de cada controle** (814) | `ui_tips_table.c` | já existia |

- O endereço da base fica em `data/branding/endpoints.ini`, seção `[assistant] base_url`.
- `REALENGINE_ASSISTANT_URL` troca o endereço por outra pasta ou URL; o teste usa a pasta gerada em `build/assistant`.

## Online e offline
- **Online** (padrão):
  - na primeira pergunta, um worker (`async_run`, com cartão) baixa o `index.json`;
  - nada é instalado e a base fica só na memória;
  - sem internet, as funções e os controles respondem sozinhos.
- **Offline:** "Baixar para usar offline" grava o `index.json` em `%APPDATA%\RealEngine\assistant\`.
  - A próxima abertura lê de lá.
  - "Atualizar" baixa de novo e "Remover" apaga.
  - O teste usa `REALENGINE_ASSISTANT_DIR` para não tocar na pasta do usuário.

## A busca (`src/assistant.c`)
- **Texto:**
  - UTF-8 vira minúsculas sem acento;
  - camelCase é separado (`PlaySound2D` vira `play` e `sound2d`);
  - palavras vazias (pt, en, es) são ignoradas;
  - o stem é leve: plural, terminações de verbo (-ando, -ar, -ing, -ed...) e a última vogal (`pulo`, `pular`, `pulando` viram `pul`).
- **Índice:** termos por hash (FNV), ordenados por documento (busca binária) e com frequência de documento numa tabela
  aberta.
- **Pesos por campo:** título e palavras-chave 3, id 2, texto 1. Nas funções: nome 3, categoria 1,5, descrição 1.
- **Ordenação:**
  - BM25 (k1 1,2; b 0,75), vezes a fração das palavras da pergunta presentes;
  - os artigos vêm na frente (×1,35), as funções ×0,9 e os controles ×0,85;
  - os resultados abaixo de 20% do melhor caem.
- **Sinônimos:** `assistant/synonyms.txt` (pular, saltar, jump; personagem, character...), também baixado: melhora sem
  build nova.
- **Erros de digitação:** uma palavra que não está na base vai para a mais parecida por trigramas (Jaccard ≥ 0,4):
  "pakage" encontra "package".
- **Nome exato** (`PlaySound2D`, `play_sound_2d`): a função ou o controle vem primeiro.
- **Mensagem de erro colada:** os artigos `kind: error` têm `match:`, um pedaço da mensagem do compilador; o que bate vai
  para o topo.
- **Fraco:** nada bateu, ou o melhor ficou abaixo de 2. A janela oferece "Enviar como sugestão": abre o Enviar uma
  Sugestão já preenchido e o usuário envia. A pergunta também fica em `assistant_unanswered.txt`.

## A janela (`src/assistant_ui.c`)
- Responde enquanto se digita; Enter busca a base na primeira vez.
- À esquerda as respostas, com o tipo (Como fazer, Receita, Erro, Função, Controle); à direita a escolhida.
- Os artigos são em Markdown: títulos, passos, pontos, código (fonte Consolas, com Copiar e barra horizontal).
- Os links viram botões:
  - `show:Build>Package Game...` (**Mostrar onde**): os controles com esse rótulo ganham um contorno pulsante por 8 s,
    com o caminho no topo da tela. O ImGui do editor avisa cada item com rótulo desenhado (`ItemAdd`,
    `re_imgui_highlight_item`, só enquanto há alvo). Um item de menu acende quando o menu abre.
  - `open:` (package, prefs, project, outliner, details, place, content, log, stats, debug, api, utilities, browser,
    platforms, modes);
  - `run:` (play, build-lighting, landscape-mode, foliage-mode, measure);
  - `add:` (formas, luzes, peças de protótipo, Player Start, câmera, neblina, nuvens, multidão, Landscape, Foliage,
    sons);
  - `api:<função>`, `article:<id>`, `url:`.
  - O editor executa as ações (`assistant_action` em `editor.c`); nada roda sem o clique do usuário.
- Primeira abertura: 60% × 72% da tela, centrada (os grafos cabem inteiros); depois vale o tamanho do usuário.
- `--show assistant:<id>` abre o Assistente num artigo (prints); se a base ainda não chegou, abre quando chegar.

## Imagens em vez de texto (`src/assistant_draw.c`)
O que não é código vira desenho, feito pelo próprio editor (ImDrawList, nada baixado, custo só com o artigo aberto):
- **Blueprint:** a variante Blueprint de toda receita é um grafo de nós como o da Unreal, lido da cadeia de texto
  (`Event Tick -> Branch ( x ) True -> Set ...`). Cada linha é uma cadeia; uma linha recuada com rótulo (`True ->`,
  `False ->`) sai do nó de cima, `-> X` continua a anterior, uma linha recuada sem seta vira texto do nó. Cores por
  tipo (evento vermelho, fluxo cinza, Set verde-azulado, função azul, entrada de função roxa), fios de execução em
  curva com o rótulo junto ao nó de destino, barra horizontal quando é largo.
- **Blocos ```` ```diagram <tipo> ````** nos artigos (36 hoje, pt e en com os rótulos de cada idioma):
  - `steps`: passos numerados com setas (`A | B | C`): começar aqui, primeiro jogo, empacotar, importar...;
  - `keys`: teclas desenhadas com o que fazem (`Ctrl+S: Salvar | ...`): atalhos, mover, voar, PIE, Landscape...;
  - `axes`: os eixos X/Y/Z com as cores do gizmo;
  - `layout`: o mapa do editor (`Nome: x y l a` em %); clicar numa parte faz o "Mostrar onde" dela;
  - `compare`: tabela (`| col | col` e linhas `nome | a | b`), sim/não em verde/vermelho: mobilidade, Blueprint ou
    código, material × textura, colisão × sobreposição, luzes, RPCs, BT × Real Brain, ECS;
  - `bars <unidade>`: gráfico de barras (`nome: valor`): FPS em ms, JIT, orçamento do quadro.
- `tools/assistant_pack.py` recusa um tipo desconhecido (sairia como texto).

## Escrever artigos
Um arquivo por artigo em `assistant/articles/`:

```
id: package-game
kind: howto | recipe | error | concept
title: Empacotar o jogo | Package the game
keys: empacotar, exe, package...
match: (só kind error) um pedaço da mensagem
--- pt
o texto em português
--- en
o texto em inglês
```

- `python tools/assistant_pack.py` confere ids, links e diagramas e gera `build/assistant/index.json` e o
  `manifest.ini`.
- **Regra (usuário, 2026-11-07): toda função nova ou novidade da engine entra no assistente no mesmo trabalho.**
  - Funções da API (nós e funções dos scripts) e descrições dos controles (`ui_tips`) já entram sozinhas.
  - O resto pede artigo: como fazer (passos e `diagram`), receita (variantes Blueprint, Real, C#, Python, C) ou conceito.
  - `assistant/coverage.txt` liga cada `notas-dev/<área>.md` aos artigos dela: uma área nova sem linha (ou com artigo
    que não existe) faz o `assistant_pack` falhar, e com ele o teste `assistant_pack`.
  - Em 2026-11-07 entraram os que faltavam: Debug Viewer, vidro/translúcidos, fratura, atualizador, idioma, tela de
    carregamento e intro, jogos de demonstração, streaming de texturas, job system (126 artigos).
- Publicar é copiar esses dois arquivos para o repositório `realengine-assistant`. Os editores pegam a versão nova na
  próxima pergunta (online) ou em "Atualizar" (offline).
  - O repositório público https://github.com/NathanLourencoPeixoto/realengine-assistant foi criado em 2026-11-07, junto
    com a 0.22 (ok do usuário). Tem `index.json`, `manifest.ini`, `articles/`, `synonyms.txt`, `coverage.txt` e um
    README.
  - O envio usa a API Git Data do GitHub, num commit só: blobs, árvore, commit e ref (script na sessão async). Também
    serve um clone comum.

## Junto com o assistente
- **Novo nó Launch Character** (o da Unreal): o `Jump` só pula do chão, então o pulo duplo, um trampolim ou um
  empurrão precisavam dele.
- **`Is Key Down` e `Get Input Axis` recebem o nome nos scripts** (`IsKeyDown("Space")`,
  `GetInputAxis("MoveForward")`). Antes só liam o Details e não tinham como ser usados por código.

## Corrigido ao escrever as receitas
- O contorno do "Mostrar onde" tinha espessura e flags trocados no `AddRect` (saía com 0 px de espessura).
- Um pino "Target" que não é ator (o vetor de `Find Look at Rotation`) era tratado como o self da chamada nos scripts.
  Agora só um Target do tipo ator é o self; os outros são argumentos (`script_lower.c` target_pin, `script.c` na
  assinatura).

## Medidas e o teste (`assistant`, `--assistant-test`)
- **Hoje:**
  - 117 artigos + 346 funções + 815 controles = 1.278 documentos, 18.086 termos;
  - base montada em ~42 ms; pergunta em 0,21 ms;
  - 30 perguntas em todos os níveis e jeitos de falar (iniciante, gíria, inglês informal, espanhol, profissional, código),
    todas certas;
  - 44 trechos de código das receitas compilados.
- **Primeira versão:**
- **Base:** 45 artigos + 346 funções + 814 controles = 1206 documentos, 14.392 termos, 2.419 palavras no vocabulário.
  Montada em ~23 ms num worker.
- **Pergunta:** 0,10-0,18 ms (média de 200).
- **O teste:**
  - 10 perguntas: português, inglês, um erro de digitação, uma mensagem de erro colada, o nome de uma função e uma sem
    sentido, que tem de dar "fraco". Todas certas.
  - As 10 receitas de código dos artigos são compiladas como scripts. Um artigo ensinando código que não compila
    reprova o teste.
  - A cópia offline é gravada e removida.
  - A janela é desenhada durante a medida.
- O teste `assistant_pack` (Python) gera a base antes, como fixture.
