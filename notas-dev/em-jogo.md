# Tudo editável em jogo (Q16, lote 5 do plano 0.43)

Pedido: mapas gerados em jogo (procedurais, PCG) recebem o bake de luz do editor; toda configuração de render, qualidade,
física e áudio muda por Blueprint no jogo; uma varredura lista o que ainda é "só editor".

Código: `runtime_lighting.c/.h` (o pedido e o fim do bake em jogo), `gi_bake.c` (modos, faixas, thread, envio em
fatias), `gi.c` (sondas do bake em passo, `gi_lightmap_make/swap`), `gi_cache.c` (trava do `template_for`), `render.c`
(timestamps 28–29 do bake), `props.c/.h` + `props_table.inc` (gerado por `tools/gen_props.py`), `cvars.c/.h`,
nós em `bp_internal.h`/`blueprint.c`/`bp_vm.c`, hosts `game_main.c` e `editor.c` (Play), teste `ingame_test.c`,
varredura `tools/ingame_sweep.py` (+ `tools/ingame_sweep_baseline.txt`).

## Build Lighting em jogo
- **Build Lighting (Async)** (Mode, Quality, Budget ms → Started; a 1ª saída segue na hora, **Completed** no fim),
  **Get Lighting Build Progress**, **Cancel Lighting Build**, **Event On Lighting Built** (Success, Seconds),
  **Save / Load Built Lighting** (`Saved/SaveGames/<nome>.rlight`: o mesmo mapa da mesma semente carrega sem bake; as
  malhas achadas pelo hash do transform).
- O mesmo bake do editor (`gi_bake.c`: lightmaps L0/L1, paredes, volume de sondas baked, bounces), três modos
  (`GiBakeMode`): **EDITOR** (como era: uma varredura do atlas por quadro, todas as sondas), **BLOCKING** (Loading
  Screen) e **PROGRESSIVE**.
- **Loading Screen**: o host congela o mundo como pausado (sistemas e `bp_tick`) e desenha `brand_splash_draw` (a tela
  de carregamento do projeto) com o progresso; a GPU faz ~30 ms de bake por quadro (até 4 varreduras; atlas até 4096,
  a densidade do editor, 10 cm).
- **Progressive**: o jogo segue; uma faixa de linhas do atlas por quadro dentro do orçamento (1 ms padrão), medida pelos
  timestamps 28–29 do próprio bake (média do custo por linha; nunca mais que 1,5× as linhas do quadro anterior); as
  sondas do bake em passo 4 (o custo das dinâmicas); os lightmaps **escondidos** até o fim (`gi_lightmaps_shown`): a GI
  dinâmica (cache de superfície + sondas) ilumina o nível enquanto isso, e a troca é de uma vez no fim. Atlas até 2048
  (texel cresce: 4× menos para enviar).
- **Sem travada no início** (os dois modos do jogo): coletar as malhas no quadro (rápido); numa **thread**: os charts e o
  empacotamento (`template_for` do cache de superfície com uma trava), o G-buffer na CPU, os buffers da GPU (`gpu_buf_create`
  não tem estado compartilhado) e as imagens novas dos lightmaps; no quadro seguinte ao fim da thread (`gi_bake_frame`,
  pelo `rtl_frame`, antes do render: nunca com o set de descritores já ligado) a troca das imagens com a GPU ociosa e o
  rebind; depois o G-buffer vai para a GPU em fatias (6 MB por quadro jogando, 64 MB na tela de carregamento) por um anel de
  3 buffers de staging. Os quadros só começam o bake depois (e começam com 32 linhas: a 1ª varredura também acha os
  texels enterrados e as paredes; o atlas inteiro numa vez dava ~0,7 s de GPU num quadro).
- No Play do editor: `game_bake_tick` (o cartão "Building lighting (game)" com Cancelar), o mundo congelado no modo de
  carregamento; ao parar o Play, `rtl_end_play` volta a luz do nível (`.rlight`) e o `bake_tick` do editor nunca grava o
  bake do jogo como o do nível.
- Sem ray tracing por hardware: Started false e o evento com Success false no quadro seguinte (o jogo segue com a GI
  dinâmica por software ou o céu).

## Propriedades por nome
- **Set / Get Property by Name** (Target vazio: self; o ator ou o primeiro componente anexado que tem): o rótulo do
  Details ("Intensity", "Max Walk Speed"), com seção ou componente ("Light.Color", "Character Movement.Max Walk Speed"),
  espaços e caixa não importam, o parêntese pode faltar ("Max Force"). Value para número/checkbox, Vector para vetor/cor.
  **List Properties** lista os do ator. Mudar marca o ator (`ecs_touch`).
- A tabela **é gerada do código do Details**: `tools/gen_props.py` lê `row_float/row_slider/row_check/row_color/vec3_row`
  do `editor.c` e os `igDragFloat/igCheckbox/...`/`flag()` dos painéis `*_ui.c`, resolve o ponteiro até o campo do
  componente (structs dos `.h`, `g_components` do `ecs.c`, locais copiados de volta `if (row(&x)) c->f = x`, bits com
  máscara) e escreve `src/props_table.inc` (`P(...)` campo, `B(...)` bit). O `props.c` confere cada tamanho com
  `_Static_assert` (um campo mudou de tipo: `python tools/gen_props.py`). 487 propriedades + Mobility.
- Checkboxes que copiam um bit por uma variável local entram pela lista `EXTRA_BITS` do gerador (Show Icon, Collision
  Enabled, molas e drives das juntas, Post Process Enabled/Unbound, Horror Exposure, Camera Motion Blur, Groom LOD/
  Dynamic/Collide, Use First Person Camera, Can Repeat, Ability Keys/Bar).

## Console variables
- **Set / Get Console Variable**, **Execute Console Command** ("r.ScreenPercentage 75" define, "r.ScreenPercentage"
  lê), `--cvar nome=valor` no jogo. 86 nomes em `cvars.c`: as opções do jogador (sg.Scalability, r.ScreenPercentage,
  r.VSync, t.MaxFPS, au.*Volume, input.*), todo campo de `GiSettings` (r.GI.*, r.Reflections, r.SSR, r.AmbientOcclusion,
  r.Shadow.Virtual.*, r.Shadow.Local.*, r.LOD.ErrorPixels, r.TSR.Scale...), r.MSAA, os de render do projeto
  (r.RenderStyle, r.FilmGrain, r.Stylized.*, r.Video.*, r.Upscaler...), física (p.GravityZ, p.MaxSubsteps,
  p.SolverSubsteps), áudio (au.MaxChannels, au.Binaural) e saves (save.*).
- O host liga o seu view (`cvar_bind_view`: o `RenderView` do jogo, o `ed->gi/msaa` no Play); um valor pedido antes
  (o BeginPlay do jogo roda antes do view existir) fica guardado e entra no bind. No Play do editor, `cvar_begin_play`/
  `cvar_end_play` devolvem o view e o projeto ao parar (nada vaza para o editor nem para o `.rproject`).

## A varredura (`tools/ingame_sweep.py`, teste `ingame_sweep`)
Lista o que não tem caminho em jogo: controles do Details, campos do Project Settings sem cvar (fora as definições do
projeto: nome, mapas, classes, empacotamento, plataformas, rede) e sistemas que o quadro do editor roda e o do jogo não.
`--check` falha se `props_table.inc` estiver velho ou se aparecer um item novo fora de `tools/ingame_sweep_baseline.txt`
(`--write-baseline` aceita a lista de hoje de propósito); `--markdown` escreve a lista.

Hoje (552 controles do Details, 491 alcançáveis; Project Settings todos cobertos; nenhum sistema faltando):
- **Foliage types** (Color, Density, Stretch, Max Slope, Wind): são do asset de folhagem, não de um componente.
- **Conversões de unidade feitas à mão** (o campo cru é alcançável pelo nome dele, mas o rótulo mostra outra unidade):
  Diaphragm Blades, Shutter Speed, World Partition (Cell Size, Loading Range, HLOD Triangles/Min Size, em m), Landscape
  Size, Prototyping Steps/Ceiling, Crowd Agents, Rim Height, Inventory Slots, Pickup Count.
- **Rótulos que não são texto fixo** (pontos de spline de trilho/rio, multiplicadores de dano por tag, os overrides do
  Post Process da câmera, alguns checkboxes "##"): os Post Process têm o nó **Set Post Process Setting**.
- **Já alcançados por um nó próprio**: Tick When Paused (Set Tickable When Paused), Actor Outline Color (Set Actor
  Outline), Flicker Amount (Set Light Flicker).
- **Do próprio editor** (vista, pincéis, jogadores do Play: não existem no jogo) e **sistemas do editor por natureza**:
  Construction Scripts editando, Take Recorder, painel Levels, Project Settings, cartões de tarefas, prévia do editor
  de veículos, overlay de custo por ator.

## Medidas (RTX 3060, 1440p, GPU livre; o mapa: a Instalação do procgen gerada no jogo, 10 salas de 8 m, 79 malhas Static, 11 luzes)
| | Loading Screen | Progressive |
|---|---|---|
| Atlas | 4096² (10 cm, a do editor) | 2048² (20 cm) |
| Thread (charts, G-buffer, buffers e imagens da GPU) | 150 ms | 58 ms |
| No quadro: troca dos lightmaps + rebind | 64 ms (atrás da tela) | 14–16 ms |
| Bake até ficar pronto | **4,9 s** | **4,0 s** |
| GPU do bake por quadro | 25,7 ms (máx. 39) | **1,21 ms** (orçamento 1,0; máx. 4,6) |
| Quadro durante o bake | 24 ms (tela de carregamento animando) | **3,78 ms** médio (2,73 sem bake), pior **15,7 ms** |
| Depois do bake | 362 fps | 363 fps |

- Antes da thread e do envio em fatias o início custava **1026 ms** num quadro (atlas 4096, G-buffer de 1,3 GB enviado
  de uma vez); o controlador pela média de linhas/ms passava do orçamento 2,6×.
- Nós genéricos (o trabalho do nó sem a VM): **Set Property by Name ~90 ns, Get ~85 ns** (num componente anexado ~90 ns),
  **Set Console Variable ~200 ns, Get ~200 ns** (`ingame_test.c`, 100 mil chamadas).

Testes: `ingame_package_loading/progressive` (props/cvars em C e medidos, o nível com a Instalação e `BP_InGameTest`,
empacotado), `game_bake_loading` e `game_bake_progressive` (o jogo empacotado: cvar e propriedade OK, bake até
Completed e o evento; progressivo sem quadro acima de 25 ms e 200 fps depois), `ingame_sweep` (área blueprints).

## Falta
- A troca dos lightmaps no fim do bake ainda espera a GPU (14–16 ms num quadro jogando): reservar as imagens antes, ou
  atualizar só os bindings dos lightmaps (`update_descriptors` refaz o set inteiro).
- A transição da GI dinâmica para os lightmaps é de uma vez (sem cruzamento); visualmente perto, mas não idêntica.
- Bake por software (sem RT por hardware) e bake incremental (só as salas novas de um mapa que cresce).
- Os controles da lista acima com conversão de unidade: um rótulo próprio ("Cell Size (cm)") ou a conversão na tabela.
