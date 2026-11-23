# Subníveis e Lighting Levels empilháveis

Pedido (2026-11-16): "como na Unreal tem um sistema de sublevel no painel, vamos criar isso também, mas também tem a
possibilidade de light-level ... na Unreal só pode ter 1 light level por vez, aqui vamos poder ter vários um por cima do
outro como um add".

## O que é
- **Janela > Níveis** (`editor_levels.c`): o painel Levels da Unreal. Nível Persistente + subníveis (outros `.rlevel` do
  projeto carregados no mesmo mundo). Por linha: olho, cor, nome (`*` alterado), Lighting, Carregado no Início, Travar,
  atores. Duplo clique = nível atual (os atores novos vão para ele). Botão direito: Tornar Atual, Mover Atores
  Selecionados, Selecionar Atores, Salvar Nível, Remover. Botões: Adicionar Existente, Criar Novo, Salvar Níveis.
- **Lighting Level**: subnível marcado Lighting. O Build Lighting faz um bake por grupo de luzes; o que aparece é a soma
  dos visíveis. A Unreal (Lighting Scenarios) só deixa um ligado; aqui qualquer combinação, porque a luz é linear.
- Nós: **Set Lighting Level Visible** (nome, ligado) e **Is Lighting Level Visible**.

## Como funciona
- **Lista** `<nível>.rlevels` (texto ao lado do nível): `nome|visível|carregado no início|lighting|travado|r g b` por
  linha, mais `current=N`. Os subníveis são achados pelo nome (`level_find`), então o pacote acha também.
- **Carregar** (`sublevels.c`): `level_stream_copy` com id `SUBLEVEL_ID0 + slot` (1..32; o streaming do Play usa 1001+).
  Os atores levam `CStreamed.level`. O nível de um componente é o do seu dono (`owner_root` pelo `CAttach`).
- **Olho**: `TRANSFORM_HIDDEN` + `TRANSFORM_LEVEL_HIDDEN` (bit 6) só nos atores que não estavam escondidos. Ao mostrar,
  só sai o que o nível escondeu: um ator escondido por si continua escondido. O bit nunca vai para o arquivo (o save
  limpa).
- **Salvar**: o mundo é serializado e lido num `World` temporário (mesmos handles, os donos continuam certos), os atores
  que não são daquele nível são destruídos, as marcas saem, e `level_save`. O persistente sai sem os atores dos
  subníveis; cada subnível alterado no próprio arquivo; a lista por último.
- **Atores novos no nível atual**: em vez de mexer em cada caminho de spawn, um "visto" por índice de entidade (4 MB,
  só alocado quando um subnível vira o atual). O scan só roda quando o atual é um subnível e o número de vivos mudou.
  Undo, abrir e Stop resincronizam (o que voltou não é novo). Duplicar mantém a marca de origem.
- **Travar**: `editor_select` recusa o ator de um nível travado.
- **Play / jogo**: `sublevels_begin_game` tira os não "Carregados no Início", tira o olho do editor, registra os nomes no
  streaming (Load/Unload Stream Level os acham) e soma a iluminação. No Stop o snapshot volta e o conjunto do editor
  também. O pacote leva o `.rlevels` (e os subníveis já iam como os outros níveis do projeto).

## Build Lighting em etapas
1. Todos os subníveis ficam visíveis durante o bake (os escondidos também bloqueiam e iluminam).
2. **Base**: luzes do persistente e dos subníveis comuns; as dos Lighting Levels escondidas
   (`sublevels_lights_only(-1)`). Salvo em `<persistente>.rlight`.
3. **Cada Lighting Level**: só as luzes dele, e `gi_bake_set_no_emissive(true)` → `GI_NO_EMISSIVE` no quadro: os raios
   do bake (lightmap e volume de sondas, `rt.glsl`) não veem emissivo, senão ele seria somado uma vez por bake. Salvo em
   `<subnível>.rlight`.
4. No fim as luzes voltam e o editor mostra a soma. Cancelar devolve as luzes (`bake_watch`).
- O céu (sky light) agora respeita o olho do editor (`render.c`), como as outras luzes já faziam (`lights.c`): sem isso,
  o bake de um Lighting Level teria o céu de novo.
- As malhas do atlas não dependem da visibilidade (`gi_bake_start` pega todas as Static), então os bakes têm o mesmo
  atlas e as mesmas entradas. Um `.rlight` com outras entradas fica fora da soma (aviso no Output Log): Build Lighting de
  novo.

## A soma (`gi_lightmaps_load_sum`, `gi_bake.c`)
- L0 (RGB) somado, alfa (válido) = máximo; L1 (direção) = normalize(dir_a·lum_a + dir_b·lum_b); paredes = as do primeiro;
  irradiância do volume (os 2/3 iniciais) somada.
- Os `.rlight` lidos ficam em memória (até 1,5 GB; relidos quando o arquivo muda). A soma é F16C/AVX2 (`_mm_cvtph_ps`),
  com C puro no Android. Sobe para a GPU direto da memória (`lightmaps_upload`), sem arquivo no meio.

## Medidas (RTX de desenvolvimento, atlas 1024², 5 malhas Static, `--levels-test`)
| | antes | depois |
|---|---|---|
| soma base + 1 Lighting Level | 79 ms (arquivo temporário, half a half) | **21 ms** (em memória, F16C) |
| desligar um Lighting Level (Set Lighting Level Visible) | 26 ms | **10 ms** |
| ligar de novo | — | **19 ms** |
| custo por quadro do painel/scan sem subnível atual | — | 0 (retorna antes) |
- O que sobra é a subida para a GPU (staging de 24 MB, espera da fila). Para trocas no meio da jogabilidade num atlas
  4096: fazer a troca numa transição (fade, porta) ou, depois, somar na GPU com um compute.
- Bake da base 2,84 M (L0 somado) e da noite 24,6 k: a lâmpada só aparece no bake dela (o sol e o céu não).

## Testes
- `levels` (área `niveis`, `levels_test.c`): cria, coloca atores no atual, salva, abre, confere arquivos e marcas, olho,
  trava, mover para o persistente, Build Lighting em duas etapas, soma, liga/desliga pelo mesmo caminho do nó.

## Blend dos Lighting Levels (2026-11-16, "um node Set Lighting Level Blend que possa ser dinâmico")
- Cada bake (persistente e cada Lighting Level com `.rlight`, carregado ou não) vira uma **camada na GPU**: L0, L1 e a
  irradiância do volume de sondas, num buffer só (`gi_lightmaps_load_layers`). `lightmap_blend.comp` escreve a soma
  ponderada direto nas imagens do lightmap (bindings 11/12) e no atlas das sondas (binding 5). Roda só nos quadros em que
  um peso mudou (mais 2, porque o volume sobe um quadro depois). Texels sem superfície ficam de fora.
- As **luzes** do nível vão no mesmo peso (`lights_set_level_weight`, multiplicador por id de `CStreamed` em
  `lights_gpu`, e o sky light em `render.c`). Elas são tocadas para o buffer de luzes pegá-las de novo. Peso 0 esconde o
  nível (sem custo de sombra).
- Nós **Set Lighting Level Blend** (nome, peso: 0 desligado, 1 como no bake, >1 mais forte) e **Get Lighting Level
  Blend**. Set Lighting Level Visible passa pelo mesmo caminho e guarda o peso. No painel, ao lado de Lighting, um campo
  de peso ao vivo, salvo no `.rlevels` (8ª coluna).
- **Assíncrono**: um Lighting Level ainda não carregado tem o arquivo lido num worker (`async_run`). Os atores entram na
  thread principal um ou dois quadros depois (`load_done`). As camadas de luz já estão na GPU desde a abertura do nível,
  então ligar e desligar nunca lê arquivo.

| Medida (atlas 1024², 2 camadas, RTX 3060) | antes (soma na CPU) | agora |
|---|---|---|
| ligar/desligar ou mudar o peso (CPU) | 10–19 ms | **0,001–0,003 ms** |
| GPU por quadro com o peso animado sem parar | — | **0,149 ms** (0,215 sem pular os texels vazios) |
| GPU com o peso parado | — | 0 |
| memória | — | 25 MB por camada (atlas 1024) |
- Medir: `REALENGINE_BLEND_EVERY_FRAME=1` + `--gpu-passes` ("Lighting Levels (blend)").

## Q3 do plano 0.43 (2026-11-17): o caso do usuário, níveis pretos, Outliner por nível, remover, painel novo
Pedido: "Lighting Levels Day e Night com predefinições de céu, ligar/desligar não soma"; "níveis pretos ao reabrir";
"Outliner mostra só o nível selecionado"; "remover subnível visível"; "painel Níveis redesenhado". Relatórios da Real
Engine lidos antes: nenhum bug de níveis aberto (#135-137, o crash ao adicionar subnível, já fechados na 0.42).

### (a) Por que não somava (reproduzido no teste `levels_lighting_sum`)
1. **A predefinição de céu ignorava o nível atual** (`editor_sky_preset`, `editor.c`): procurava o sol, o Sky Light, a
   neblina e as nuvens no mundo inteiro. Com o Night atual, a predefinição Night achava o sol do **Day** e o virava
   lua; o Night ficava vazio. Agora `find_level_actor` só olha o nível atual (`levels_in_current`) e o que falta nasce
   nele na hora (`levels_assign_now`).
2. **Só as luzes seguiam o nível.** A neblina e as nuvens eram "a primeira do mundo" (`weather_fill_frame`), mesmo
   escondidas; o Post Process Volume não via o nível; o sol do céu era "o primeiro directional" sem o peso.
   - **O peso do nível é a parte dele em tudo o que ilumina** (`lights_level_weight`): 0 quando desligado (também no
     jogo, onde o olho do editor não vale; antes o peso voltava a 1 e as luzes do nível desligado continuavam no jogo).
     `lights_gpu` recusa peso 0 (sem luz e sem sombra).
   - **Neblina**: a mistura de todas as visíveis, cada uma no peso do seu nível: as densidades somam (meios que se
     somam), o resto (altura, queda, albedo, anisotropia, distâncias) pela parte de cada uma; emissivo pela média.
     **Nuvens**: a forma da camada pela média ponderada, a quantidade (cobertura, sombra, cirrus) pelo peso (somado até
     1). **Local Fog Volumes**: densidade vezes o peso. Escondidas pelo olho do nível (`TRANSFORM_LEVEL_HIDDEN`): fora.
     Uma neblina ou uma nuvem no peso 1: igual a antes.
   - **Post Process Volume**: o peso do volume vezes o do nível; nível desligado: fora.
   - **Sol do céu** (`render.c`): o directional mais forte já no peso do nível, com o lux vezes o peso (Day e Night a
     0,5: o céu do dia a meia força, não o primeiro achado).
3. **O bake de cada Lighting Level leva só o que ilumina nele**: `sublevels_lights_only` agora esconde também neblina,
   volumes de neblina, nuvens e Post Process dos outros níveis (`LIGHTING_ACTORS`). Durante o Build Lighting todos os
   níveis ficam no peso 1 (`sublevels_bake_weights`): um nível com mistura 0,5 não sai com metade da luz no bake (e
   depois metade de novo).
4. `sublevels_apply_all`: todos os pesos de novo ao abrir, ao remover (o slot que ficou vago volta a 1) e no fim do
   bake; as luzes tocadas uma vez só.

### (b) Níveis pretos ao reabrir
O `.rlight` guardava cada malha pelo **handle da entidade** e por um hash com o **slot da malha na GPU**. Ao reabrir, o
persistente entra primeiro e os subníveis depois (`level_stream_copy`): os atores dos subníveis voltam com outro
handle (no teste: o cubo do Day era a entidade 10, depois 1048586) e as malhas importadas podem ter outro slot. Sem
achar a entrada, a malha ficava sem lightmap (preta).
- A chave do bake agora é estável: transformação + **id do asset** da malha (`gi_stable_hash`, `mesh_key`).
- `gi_lightmap_for(e, t, slot)`: pela entidade primeiro; errou, pela chave (um segundo índice ordenado por hash,
  `g_byhash`, montado com as entradas). Aceita o hash antigo também: os `.rlight` de antes da 0.43 continuam valendo.
- O teste reabre e confere que **todas** as malhas Static acham o lightmap (5 de 5) e as 3 camadas na GPU.

### (c) Outliner por nível
- Com subníveis, o Outliner ganha a linha **"Nível: <nome> ▾"** (ícone na cor do nível; **Todos os Níveis**, Nível
  Persistente, cada subnível). Clicar num cartão do painel Níveis escolhe o nível; clicar de novo volta a Todos.
- A lista é refeita só quando o nível escolhido muda (`levels_outliner_revision`), como já era com o filtro de texto;
  com um nível escolhido a lista é plana (sem pastas), como na busca.

### (d) Remover subnível
- Quatro caminhos: a **lixeira** em cada cartão, o botão **Remover** no topo (o nível escolhido ou o atual), clique
  direito > Remover Nível Selecionado e **Del** com o painel em foco (o Del do editor não apaga os atores selecionados
  nesse caso: `levels_panel_focused`).
- Sempre um diálogo: nome, quantos atores saem, o arquivo fica, "Ctrl+Z desfaz" (Enter confirma, Esc cancela).
- **Desfazer**: cada passo do undo (a cópia do mundo, `editor.c`) leva também a lista de subníveis
  (`levels_undo_take/apply`, só quando há subníveis). O mundo volta com os atores e as marcas `CStreamed` daquele
  momento e a lista junto, então os dois sempre combinam. Ctrl+Y remove de novo. Antes remover limpava o undo.
- Os slots de depois descem com `renumber` (uma passada pelo mapa id antigo → novo).

### (e) Painel Níveis em cartões
- Um cartão por nível: faixa na cor do nível, olho, ícone do tipo (**sol** = Lighting Level, **dados** = nível comum;
  clique troca), nome (`*` alterado, "(faltando)"), "atual" e borda destacada no nível atual, contagem de atores
  (também a do persistente), mistura (0-2) nos Lighting Levels, **No início**, cadeado desenhado (`draw_lock`), cor,
  lixeira. Cores do estilo do ImGui (`ImGuiCol_*`), nada de cor fixa nova além dos ícones (para o Q1 trocar por tokens).
- Clique: Outliner por nível; clique duplo: atual; **arrastar** um cartão: reordena (`move_slot`, marcas renumeradas,
  undo); **soltar atores** do Outliner (`RE_OL_ACTORS`) num cartão: move para aquele nível.
- "Criar Novo..." com a opção **Lighting Level** já marcada.

### Medidas (RTX 3060, `levels_lighting_sum`/`levels_remove`, Release)
| | |
|---|---|
| bakes do caso do usuário (L0 somado) | persistente 0,0; Day 4 264 187; Night 7,9 |
| remover um subnível (3 níveis, 6 cubos; inclui salvar o alterado e recarregar a iluminação) | 2,2 ms |
| ligar/desligar um Lighting Level (CPU) | 0,008 ms (como antes) |
| neblina/nuvens por quadro | um laço só pelos arquétipos com Fog/Cloud (antes parava no primeiro): poucos atores, sem custo medível |
| painel e Outliner por quadro | contagem 2x por segundo (como antes); a lista do Outliner só refeita quando o nível muda |
- Correção junto: `lights.c` mandava a mesma luz duas vezes num quadro quando era tocada duas vezes (o peso de um nível e
  depois o de todos): regiões sobrepostas num `vkCmdCopyBuffer` (VUID-vkCmdCopyBuffer-pRegions-12482, o teste `levels`
  acusou). `regions_dedupe` deixa só a última cópia de cada slot.

### Testes (área `niveis`)
- `levels_lighting_sum` (`REALENGINE_LEVELS_TEST=sum`, `levels_q3_test.c`): o caso do usuário inteiro, com bake (RT).
- `levels_remove` (`REALENGINE_LEVELS_TEST=remove`): Outliner por nível, arrastar, remover, undo/redo, salvar e abrir.
- `levels` continua passando.
- Espanhol: a Real Engine só tem en e pt (`LANG_COUNT`); os textos novos estão em en/pt, o artigo em pt/en.

## O painel Níveis conferido aberto (sobra do Q3, 2026-11-19)
Pedido: abrir o editor (2º monitor) e conferir de verdade Janela > Níveis — cartões, olho, travar, No início, nível atual,
mover atores, Lighting Levels empilháveis, remover com confirmação e Ctrl+Z — com imagens; corrigir o que estiver errado
ou feio.

### Como foi conferido: teste `levels_panel` (`REALENGINE_LEVELS_TEST=panel`, `levels_q3_test.c`)
- O editor abre com o painel na frente (aba ao lado do Organizador), três subníveis (Lighting_Day e Lighting_Night como
  Lighting Levels, Props comum) e um cubo no persistente. Os controles são clicados **por eventos de mouse injetados no
  ImGui** (`ImGuiIO_AddMousePosEvent/AddMouseButtonEvent`) no lugar onde cada cartão os desenhou no quadro anterior
  (`levels_test_rect`, gravado só com o gancho do teste ligado), e as teclas do mesmo jeito: o caminho do usuário, não as
  funções por trás.
- Os 14 passos: olho (esconde o Night), cadeado e No início do Props, clique duplo no cartão do Day (atual), o ícone do
  tipo do Night (Lighting → Data → Lighting), o olho de volta (dois Lighting Levels ligados), o cubo do persistente movido
  para o Props (pela função do menu do cartão: o menu de contexto não tem retângulo gravado), clique no Props (o Organizador
  só com ele), **Del** (o diálogo), **Enter** (removido, atores fora), **Ctrl+Z** (de volta), a lixeira do Night e **Esc**
  (mantido). Uma imagem por passo com `REALENGINE_LEVELS_SHOTS=<pasta>`.

### O que estava errado ou feio (corrigido em `editor_levels.c`)
- O botão **Remover** do topo ficava além da borda do painel (a largura padrão não cabe os quatro): agora um botão que não
  cabe vai para a linha de baixo (`toolbar_next`).
- O puxador do slider da **mistura** cobria a palavra "mistura" e depois o número: agora o rótulo "Mistura" ao lado e um
  campo de arrastar (`igDragFloat`, 0-2) sem puxador.
- O rodapé (nível atual, Lighting Levels ligados) era cortado na borda: agora quebra a linha, e a lista de cartões deixa o
  espaço que ele ocupa.
- "1 atores" no cartão e "1 atores movidos para …" na barra de status: singular ("1 ator", "1 ator movido para …").
- O diálogo **Remover Subnível** tinha uma linha da largura da tela: agora quebra em ~30 caracteres de fonte.
- Funcionou certo de primeira: olho (ícone riscado), cadeado (laranja e fechado, os atores não se selecionam), No início,
  atual (borda e "atual"), clique (cartão destacado, Organizador por nível), Del/Enter/Esc no diálogo, Ctrl+Z e Ctrl+Y,
  contagem de atores (persistente 5 → 4 ao mover o cubo; Props 3 → 4).
- Fica para depois (não é defeito do painel): com o layout padrão, Níveis e Organizador são abas do mesmo lugar, então
  "o Organizador por nível" não aparece junto com o painel; arrastar a aba para baixo do Organizador resolve.

| Medida (`levels_panel`, RTX 3060, 1440p) | |
|---|---|
| custo do painel por quadro | o mesmo de antes (contagem 2× por segundo; os retângulos só com o gancho do teste) |
| o teste inteiro | 14 passos em ~6 s, 12 imagens |

## Pré-carregar níveis com velocidade (2026-11-23)
Pedido: "no lobby, o mapa enorme carrega de pouquinho em pouquinho para não travar; quando todos apertam play, se já
carregou vai direto, senão uma tela de loading e a velocidade sobe; o mundo de fundo não dá BeginPlay; a troca limpa,
como um corte de câmera".
- **`src/level_preload.c/h`** (até 4 ao mesmo tempo): uma thread de prioridade baixa lê o `.rlevel` em fatias de 256 KB
  com uma pausa pela velocidade (`gasto x (1/v - 1)` + piso), faz o parse num `World` à parte (só cópia de linhas: nada
  desenhado, nenhum BeginPlay) e passa o `.rlight` pelo cache do SO; a thread principal monta as árvores de componentes
  dos Blueprints dele aos poucos (`level_preload_update`, orçamento `0,08 + v² x 8 ms` por quadro).
- **Troca**: `level_preload_take` no Open Level do jogo (`game_main.c`) e do Play (`pie_open_level`): o `World` pronto
  entra no lugar do atual (cópia da struct, revisão subida para o renderer ler tudo), depois os BeginPlay como antes. Não
  terminou: a velocidade vai a 1 e termina ali, com a tela de carregamento no jogo (`brand_splash_frame`).
- **Velocidade**: por nível (`Set Level Load Speed`) ou geral (`Set Async Loading Speed`: os pré-carregamentos sem
  velocidade própria e a leitura do Load Stream Level, agora em fatias). 1 é o comportamento antigo.
- **Nós** (Game|Level Streaming): Preload Level, Set Level Load Speed, Set Async Loading Speed, Get Level Preload
  Progress (Ready, Preloading), Cancel Level Preload.
- **Teste** `preload` (`--preload-test <pasta>`): um nível de 40 000 atores (6,9 MB).
- **Medidas**: velocidade 0,05 pronto em 213 ms com a thread principal a no máximo 0,13 ms por quadro; velocidade 1 em
  9 ms (0,87 ms no quadro mais caro); a troca em **0,057 ms** com os 40 000 atores e a câmera do arquivo; pego antes de
  terminar: acabou em 5 ms na velocidade máxima, 3 quadros da tela de carregamento.
