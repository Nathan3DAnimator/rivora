# Renderer (render.c, shaders/)

Forward PBR em Vulkan 1.3, a cena do viewport em alvos próprios e depois composta no swapchain, embaixo da UI.

## Passes (cada um com timestamp de GPU: Window > Stats, ou o relatório do `--bench`)
0. **GI** ([gi.md](gi.md)): TLAS, raios e atualização das sondas (ou o volume baked de volta aos atlas), passo do
   Build Lighting enquanto ele roda.
1. **Sombra** da primeira luz direcional com Cast Shadows: mapa 2048² D32, caixa ortográfica em volta dos que projetam
   sombra (limitada a 120 m da câmera), centro preso ao texel (não tremula), bias de inclinação + normal offset,
   PCF 3x3 com comparação por hardware.
2. **Principal** (MSAA 1/2/4/8 conforme Settings > Engine Scalability, resolve para RGBA16F): malhas instanciadas
   (um draw por malha), céu procedural onde a profundidade ficou vazia, grade do editor (plano z = 0 por pixel,
   linhas com derivadas, escreve profundidade). Z invertido e projeção infinita (precisão longe).
3. **Máscara** da seleção (R8).
4. **Exposição automática**: histograma do HDR (1 pixel em 4x4) e adaptação, num buffer que o composite lê.
5. **Composite** no retângulo do viewport: exposição (manual ou automática vezes a compensação), ACES (Narkowicz, com o 0,6 de entrada), sRGB, contorno laranja
   (8 taps, só com seleção).

## Dados
- Vértices e instâncias lidos por **buffer device address** (vertex pulling; sem vertex input nem descriptor de
  buffer). Vértice 32 B (posição, normal octaédrica snorm16x2, uv, uv2 do lightmap). Instância 112 B (3 linhas afins,
  base/metal, emissivo/rugosidade, ids: entidade, flags INST_, malha; retângulo do lightmap). Por frame em voo: buffer
  do frame (campos da GI, 32 oclusores do AO de contato, endereço da exposição, luzes até 256) e de instâncias.
- Cena persistente na GPU (um slot por entidade, só o que mudou é enviado), culling e LOD na GPU, draws indiretos
  (um por forma e LOD), dois TLAS (estático e dinâmico): [escala.md](escala.md).
- Formas básicas geradas na inicialização (Cube, Sphere 48x24, Cylinder e Cone 48 lados, Plane): 1599 vértices.

## Iluminação
- GGX + Smith correlacionado + Schlick; Lambert. Directional (irradiância), Point/Spot (inverso do quadrado em metros
  com a janela suave da Unreal no raio de atenuação; cone suave), Rect (ponto mais próximo do retângulo, emissor de
  um lado). Sky Light: SH ordem 2 do céu procedural (calculado na CPU só quando o sol muda) para o difuso e o próprio
  céu (borrado para a irradiância com a rugosidade) no especular, com a BRDF de ambiente aproximada (Karis).
- Unidades físicas, céu atmosférico, exposição EV100 e sombras por raio: [luz.md](luz.md).
- Indireta: lightmap (Static após Build Lighting), senão sondas (dinâmicas ou o volume baked), senão o SH do céu;
  vezes o AO de contato. Especular: céu escurecido onde a GI diz que ele está escondido, ou reflexo por raio.
  Detalhes e medidas em [gi.md](gi.md).

## Modos de visualização (toolbar do viewport ou teclas 1–9, 0)
Unreal: Lit, Unlit, Wireframe, Lighting Only, World Normal, Roughness, Metallic. Blender: Solid (luz de estúdio,
cinza), Solid (Random) (cor estável por entidade), Texture (só a cor base/texturas, sem luz). GI: Path Tracing (Shift+P), GI Only (só a luz indireta).

## Medidas (RTX 3060, viewport ~2100x1350 dentro de 2560x1417, Release, mailbox, nível padrão: 5 malhas, 3 luzes)
Antes da GI (com a GI, por preset: [gi.md](gi.md)).
| | ms |
|---|---|
| Sombra | 0,015 |
| Principal (MSAA 4x) | 0,43 |
| Máscara | 0,008 |
| Composite | 0,36 → **0,06** (contorno com 8 taps e só com seleção) |
| CPU do renderer | 0,02 |
| Frame inteiro | ~0,67 ms, **~1500 fps** (CPU 0,25, GPU 0,59) |

## Falta
- Cascatas de sombra (CSM) para cenas grandes; sombras de point/spot/rect.
- Culling e LOD na GPU (draw indirect) quando houver malhas importadas; texturas bindless (`.rasset`).
- TAA/TSR sem ghosting (o usuário não quer o ghosting da Unreal): hoje MSAA.

## Visualizações das tecnologias (2026-10-31; menu do modo de visualização, `--view nome`)
Como os menus Nanite/Lumen da Unreal, em três seções (shader `mesh.frag`, ramos uniformes: custo nulo no Lit):
- **Geometria Real (como a Nanite)**: **LOD** (o LOD que o cull da GPU escolheu por instância: verde 0, lima 1, laranja 2,
  vermelho 3; `mesh.vert` passa `o_lod`), **Triângulos** (uma cor por triângulo de cada instância), **Mobilidade**
  (Static branco, Movable azul, em movimento laranja: fora dos raios da GI).
- **RealGI (como a Lumen)**: **Cache de Superfície** (a luz que o cache guarda, direta + indireta, sobre a cor base, com a
  grade dos texels; vermelho escuro = texel enterrado; magenta = sem quadro, as sondas iluminam), **Origem da GI** (verde
  lightmap, azul cache, amarelo sondas, vermelho só o céu; mais escuro onde a GI de contato tira luz), **Densidade do
  Lightmap** (cm por texel do atlas que a superfície lê: verde no alvo — 10 cm lightmap, 15 cm cache —, vermelho mais
  denso, azul mais grosso; grade dos texels).
- **Desempenho**: **Complexidade de Luz** (luzes avaliadas por pixel, mapa de calor até 16) e **Custo do Shader** (soma
  ponderada do trabalho do pixel: luz 1, raio de sombra 4, leitura da GI 1–4, reflexo traçado 12; calor até 48).
- Sem ray tracing a seção RealGI mostra o aviso e as outras continuam. Nomes do `--view`: lod, tris, mobility, cache,
  gisrc, density, lights, cost.

## Reflexos sem "transparência" e cores do FPS (2026-10-31)
- Antes: superfícies acima do limite de rugosidade dos reflexos traçados refletiam o céu mesmo com um objeto na frente (o
  piso sob uma esfera mostrava céu: o objeto parecia transparente no reflexo), e perto do limite o reflexo traçado era
  misturado com o do céu. Agora (Alto/Épico) toda superfície cujo reflexo ainda tem céu (rugosidade × 1,3 < 0,98) traça
  um raio: liso = o reflexo nítido; áspero = só sabe o que está na frente do céu (até 30 m) e troca a parte do céu pela
  luz do que bateu. A batida é iluminada pelo cache de superfície (direta + indireta, sem laço de luzes nem raios de
  sombra); sem quadro no cache, luzes + sondas (liso) ou só sondas (áspero). O raio áspero só onde o céu pesa no pixel
  (peso especular × parte do céu > 8%: metais, ângulos rasantes; desde 2026-11-03 relativo, ver abaixo) e a até 40 m. Custo antes desse filtro: nível padrão 1,22 → 1,56 ms no passe
  principal (506 → 483 fps); casa 444 fps.
- Barra de status e Debug Viewer: cor do FPS em gradiente — 10 vermelho forte, 30 laranja, 60 verde escuro, 100 verde
  claro forte, 200+ dourado (`dv_fps_color`).

## Pós-processamento e Post Process Volume (2026-10-31)
- **Post Process Volume** (Quick Add > Visual Effects; `COMP_POST_PROCESS`, `post.c`): caixa de 100 cm × escala ou
  Infinite Extent (Unbound), Priority, Blend Radius, Blend Weight; cada configuração com a caixinha de override da
  Unreal (Details agrupado: Bloom, Exposure, Color Grading, Lens, Motion Blur, Depth of Field, Global Illumination).
  `post_blend_at` mistura os volumes na posição da câmera por prioridade e peso; `post_fill_view` preenche o
  `RenderView` (editor e jogo). Exposição: compensação somada; Min/Max EV100 só quando um volume sobrescreve.
- Blueprints: **Set Post Process Setting** (a configuração pelo nome, escolhida no Details), **Set Post Process Blend
  Weight**, **Set Post Process Color Tint**; sem Target vale o primeiro volume do nível. Teste em `bp_test.c`.
  (Corrigido junto: Set Scalar/Vector Parameter Value on Materials agora compila o nome do parâmetro no operando c.)
- Câmera: **Camera Motion Blur** no Details (`CCamera.no_camera_blur`): desligado, só os atores que se movem borram.
  O editor fora do PIE não borra pela câmera.
- Render (`GpuFrame.pp[8]`, `prev_view_proj`; layout em `fill_post`):
  - **Bloom** (`bloom.comp`): prefiltro do HDR para 1/4 da resolução (4 taps bilineares, limiar suave, pesos de Karis
    contra vagalumes), 5 reduções de 13 taps, 5 subidas com filtro tenda; uma imagem com 6 níveis, sempre GENERAL.
  - **Velocidade** (`velocity.vert/.frag`, RG16F): só os atores do conjunto dinâmico que se moveram neste quadro,
    agrupados por malha (um draw instanciado por malha), testados contra a profundidade (sem escrever).
  - **Motion blur e DOF no composite** (variante `RE_POST` do `composite.frag`, só nos quadros com algo a borrar): a
    câmera pela reprojeção da profundidade com a view anterior (ignorada abaixo de 1,5 px de rastro), os atores pela
    velocidade; Amount como obturador no Target FPS da Unreal, Max como fração da tela; 3–8 taps por rastro. DOF por
    disco de 8/16 taps com peso pela própria CoC. Profundidade: a do prepass (oclusão ligada) ou resolvida no passe
    principal só quando o gather precisa.
  - Composite: aberração cromática, nitidez, white balance (temperatura/tint), saturação, contraste, gama, ganho,
    offset, matiz (cada passo pulado no valor neutro), ACES, vinheta, grão; AO e GI multiplicados pelo volume.
- Medidas (RTX 3060, 2560x1417, MSAA 4x, Alto; antes → depois):
  - editor vazio (`--bench`): 511 → 474 fps; bloom 0,072 ms, composite 0,054 → 0,115 ms.
  - `--sky 2`: 377 → 353 fps (bloom 0,078, composite 0,120).
  - `--bp-test` (PIE, personagem andando: motion blur da câmera e de 30 partes): 380 → 342 fps; composite 0,26 ms
    (motion blur ~0,15 ms), velocidade + profundidade ~0,05 ms. Abaixo do mínimo de 350 do teste `blueprints`.
  - O bloom é limitado pela latência dos 11 dispatches pequenos (5 níveis custam o mesmo que 6).
  - Custo aceito pelo usuário (2026-10-31): mínimos dos testes `weather_sky` 350 → 330 e `blueprints` 350 → 320
    (PIE com câmera andando e motion blur), `game_blueprints` 300 → 270 (285 fps medidos). `play_flicker` já ficava
    abaixo de 350 antes (300 fps no commit anterior): 350 → 260 (273–319 medidos).
  - Passe de velocidade com culling na CPU (frustum, raio ≥ 2 px, LOD como o cull.comp): multidão de 1000 (`anim1000`,
    24 mil partes visíveis) 62 → 90 fps; vista pequena sem custo extra.

## Path tracing jogável (2026-10-31, `shaders/pathtrace.comp`)
- O modo Path Tracing não recomeça do zero quando algo muda: o histórico é **reprojetado** (o primeiro acerto do raio
  pela matriz do quadro anterior; aceito quando a profundidade guardada lá concorda, |w| da projeção, 3%), com memória
  de 12 amostras se a câmera andou, 64 se só a cena mudou (um personagem animado), e acumulação exata (até 8192) com tudo
  parado — a referência continua valendo.
- A acumulação são duas metades lado a lado na mesma imagem (o dobro da largura): um quadro escreve uma, lê a outra;
  `.w` = amostras + profundidade (log2/32 na fração), para o teste de desoclusão sem outra imagem.
- Segundo despacho: **denoise** — vagalume (muito mais claro que todos os vizinhos) trocado pela média 3x3, depois um
  filtro 5x5 que para nas bordas de profundidade, mais forte quanto menos amostras (6/n). Teto de vagalume 16 (na tela)
  enquanto há movimento, 64 parado.
- Medido (RTX 3060, 2560x1417): blockout ao ar livre com personagem animado 245 fps (GPU 3,0 ms); casa (interior só com
  luz indireta) 150-170 fps — continua granulado com poucas amostras, como o original (é o pior caso).
- Limite: o path tracer usa a cor base dos materiais (o grafo de material não é avaliado nos raios).

## Viewport 2 — a segunda vista 3D (2026-10-31, `shaders/view2.comp`, `src/editor_view2.c`)
- Janela > Viewport 2: uma janela encaixável (ou solta, em outro monitor) com câmera própria. **Perspectiva** (botão
  direito + WASD/QE voa, roda = velocidade) ou **Topo / Frente / Lado** ortográficas (botão direito/meio arrasta, roda
  dá zoom), como na Unreal. Modos: **Iluminado** (luzes com raios de sombra, céu como ambiente com um raio de oclusão),
  **Sem Luz**, **Normais**, **Path Tracing** (acumula enquanto a vista e a cena ficam). Clique seleciona (Shift soma).
- Como: o renderer é todo de uma vista (imagens, históricos temporais, oclusão, exposição, nuvens, buffers do quadro);
  duplicar isso por vista mexeria no núcleo. A 2ª vista é um **compute de ray tracing** sobre a mesma cena da GPU (TLAS,
  cores dos materiais, luzes, céu, exposição do quadro), gravado depois da vista principal no mesmo command buffer,
  escrevendo num buffer próprio copiado para uma imagem RGBA8 (tone map ACES + sRGB) que a UI mostra. Nenhum estado da
  vista principal é compartilhado. Precisa de GPU com ray tracing (a mesma exigência da GI).
- Medido (RTX 3060, blockout jogando, janela ~900x520): vista principal sozinha 2,56-2,69 ms de GPU; com a 2ª vista
  Iluminada 2,96-2,98 ms (+0,3-0,4 ms), Path Tracing 3,10 ms.
- Testes `view2` (topo) e `view2_pt` (perspectiva em path tracing), com validação.
- Limite: o path tracing da 2ª vista recomeça pela câmera e pelas edições da cena, não por animação (um personagem
  andando deixa rastro nele); os materiais aparecem pela cor base (como no path tracer principal).

## Materiais Masked / Two Sided (2026-11-01)
- `R.mesh_masked[]`: uma malha vira "masked" quando alguma instância recebe material Masked ou Two Sided
  (`mat_cuts`). Os draws de cada lista do cull saem em faixas de malhas consecutivas (`draw_runs`): as opacas com os
  pipelines de sempre (prepass sem fragment shader), as masked com cull NONE e `masked.frag` (prepass e sombra: corta
  pelo alfa) — sem malha masked é a única chamada de antes.
- `MESH_INFO_MASKED` no `lod_count` da MeshInfo: o cull põe as masked sempre na lista early (prepass). No passe
  principal elas usam `mesh.frag` RE_MASKED_EQ: profundidade EQUAL com teste antecipado, sem gravar, sem `discard` —
  só a camada que o prepass guardou é sombreada. Sem prepass (oclusão desligada): RE_MASKED, que corta.
- Medido (Delight importado do Tlender, cabelo em dezenas de camadas de cards, RTX 3060, 2560x1417, MSAA 4x): passe
  principal 19,5 ms com `discard` em todas as camadas → 6,6 ms com o prepass + EQUAL (cabelo ocupando meia tela);
  corpo inteiro a 3 m: 203 fps.

## Occlusion auto sem engasgo (2026-11-02)
A escolha automática do pré-passe media 28 quadros sem ele a cada 3 s; com cabelo de cartões masked (a Delight) isso dá
46 ms/quadro, um engasgo periódico. Agora mede primeiro com, depois sem, e aborta o "sem" no primeiro quadro 1,5× pior;
vitória clara (sem ≥ 1,5× com) remede em 20 s. Delight ×3 no Épico: 18,1 → 15,9 ms/quadro. Sombra de área: anéis da
espiral deslocados por pixel (a grade virou grão); 4× raios na penumbra testado: +11 ms num personagem de perto.
Reflexo do céu borrado pela rugosidade (`sh_irradiance` no lugar do céu nítido acima de rugosidade ~0,5).

## O chão refletia o céu através das casas (2026-11-03)
Relatório #32: o chão do RPG (M_Grid, áspero) refletia o céu mas não os personagens nem as casas. Num ângulo rasante o
peso especular × parte do céu ficava em 5–7%, abaixo do limite absoluto de 8%: sem raio, o céu borrado aparecia por
trás de tudo. Agora o critério é relativo (`mesh.frag want_ray`): traça quando o céu refletido (peso especular × parte
do céu × radiância do céu × visibilidade do céu pela GI × AO) passa de 4% da cor do pixel. O que fica na frente do céu
(casas, personagens, a até 30 m) entra no reflexo; visto de cima (4% de um céu escuro perto do chão claro) continua sem
raio. **Trocado em 2026-11-04** (seção seguinte): o critério relativo custava 10% do passe no Alto por registradores.

## Registradores do passe principal: o critério do #32 e o spill (2026-11-04)
A main, depois dos merges da 0.15, ficou 7–10% mais lenta que a 0.14 no `house_walk` (passe main mínimo: Alto 1,50 → 1,65
ms, Épico 1,72 → 1,84). A bissecção entre builds da mesma commit não via isso: a queda veio de uma soma.
- **A causa.** O `mesh_rt.frag` (Alto/Épico) está num degrau de registradores:
  - 96 registradores com spill de 96–112 bytes para a memória local: o spill dentro das ray queries custa os 10%;
  - ou 122–128 registradores sem spill.

  O critério relativo do #32 (`sky_spec > 0,04 × pix`) manteve vivos até o raio de reflexo o peso especular em vec3
  (`f0 × ab.x + ab.y`, reusado depois) e a cor do pixel. Junto com o `jr` dos anéis do rt.glsl, isso virou o driver para
  o lado com spill. Trocar só o `mesh.frag` ou só o `rt.glsl` pelo da 0.14 não recuperava nada; os dois juntos, sim.
- **Agora.** Traça quando `(dot(f0, Y) × ab.x + ab.y) × (1 − rough_mix) × sky_vis > 5%`: o peso especular escalar ×
  quanto céu o ponto vê. Pega o chão rasante ao ar livre do #32 (5–7%) e deixa de fora o chão visto de cima (~4%) e
  lugares fechados. Com céu limpo, o chão do RPG difere do critério relativo em 0,18/255 em média, e nenhum pixel passa de
  6/255. O mesmo critério com o vec3 (`dot(f0 × ab.x + ab.y, Y)`) volta para 96 + spill.
- **`--shader-stats`** (VK_KHR_pipeline_executable_properties): imprime registradores, binário e memória local de `mesh`,
  `mesh_rt` e `mesh_rt_t`. O teste **`shader_spill`** (`--no-spill`) falha se o `mesh_rt*` fizer spill: é uma medida
  exata, que não depende de fps com a GPU disputada. Em GPU sem essa estatística, o teste passa.
- **`--bench` mostra a GPU mínima** do quadro e dos passes (`GPU minimum: ... (main, GI)`). Com outro programa usando a
  GPU, a média oscila de 90 a 320 fps, mas o mínimo fica estável em ±1%. Foi ele que mostrou a queda.
- Medido (RTX 3060, 1440p, GPU dividida com o Tlender; passe main mínimo, ms; mesma Content em todas):

| Cena | 0.14 | main antes | main agora |
|---|---|---|---|
| `house_walk` Médio | 1,218 | 1,227 | 1,228 |
| `house_walk` Alto | 1,497 | 1,629 | **1,508** |
| `house_walk` Épico | 1,722 | 1,845 | **1,734** |
| RPG Alto (`--cam -300,-600,250,60,-8`) | 1,691 | 1,851 | **1,697** (flicker 0,49% → 0,08%) |
| RPG Épico | 1,967 | 2,047 | **1,862** (flicker 0,49% → 0,10%) |
| `play_flicker` (Épico, ao ar livre) | 2,181 | 2,183 | 2,236 (128 registradores: menos pixels em voo) |

- O Médio usa o `mesh` sem RT e não caiu. Ele tem 80 registradores e spill de 48 bytes, que já existia na 0.14: é o
  próximo ganho possível para o Médio.

- **Neblina nos espelhos (2026-11-20).** O `fog_segment` (weather.glsl: a neblina iluminada ao longo do raio de reflexo
  das poças e do vidro, cena `metro`) leva o `mesh_rt`/`mesh_rt_t` de 16 para 32 bytes de spill cada. Feito no fim do
  shader (direção, distância e peso em dois registradores, `g_fr`; dentro do bloco do raio o laço fazia o mesmo spill e
  segurar quatro registradores não mudava nada). Custo medido: `house` Épico main mínimo 1,643 → 1,674 ms; `metro` Alto
  ~405 fps nos dois executáveis alternados (a média isolada oscilava 230–400 com a GPU dividida). O `shader_spill`
  agora aceita até 64 bytes no total.

### O Médio também fazia spill: céu borrado só no Alto/Épico (2026-11-04)
Pedido da sessão principal: as cenas com muitos personagens caíam muito na suíte (anim_crowd 126 → 57, crowd 330 → 203,
mm_startstop 466 → 269...). Medido com a mesma Content e pelos mínimos:
- **Não é o cache de GI.** A 0.14 já dava quadro às partes dos personagens, e os números batem: anim1000 com 7373
  quadros e +98 mil texels em movimento nas duas; ai_squad_mm com 114 e +7116.
- **É o shader `mesh`** (Baixo/Médio). O céu borrado por SH no reflexo áspero (5fc0187, anterior aos merges) levou o
  driver de 95 registradores sem spill para 80 com 48 bytes de spill: -3 a -6% do passe main. Com SH só L1, ou com o
  `sky_irr` da normal, o spill continua; só o céu nítido volta a 95. Agora o borrado fica só no `mesh_rt` (Alto/Épico),
  que segue com 128 registradores e sem spill. No Médio a parede fosca volta a mostrar a forma das nuvens, como na 0.14.
- O teste `shader_spill` agora cobre `mesh`, `mesh_rt` e `mesh_rt_t`.
- (2026-11-09) Os mapas das luzes locais (luz.md, "Sombras das luzes locais por mapa") moram numa variante própria, o
  `mesh_lsm` (80 registradores, 48 bytes de spill). Ela só é usada nos quadros em que uma luz tem mapa. O `mesh` segue
  com 95 e sem spill, e o teste não soma a variante.
- **O resto da diferença da suíte é do ambiente.**
  - A Content da build da main tem 18 MB, com 89 malhas importadas pelos testes; a da `build-release` tem 0,5 MB. A main
    faz um draw indireto por malha registrada × LOD, mesmo vazio: 680 draws contra 88 no anim1000, +10% no passe main
    (4,92 → 5,40 ms). A 0.14 faz igual.
  - O `tlender_tests` rodando na mesma GPU deixa o anim1000 entre 54 e 110 fps.

| Passe main mínimo (ms, Médio) | 0.14 | 0b31491 | agora |
|---|---|---|---|
| crowd2000 | 1,672 | 1,732 | **1,664** |
| crowd20000 | 4,019 | 4,457 | **3,889** |
| mm_startstop | 0,594 | 0,617 | **0,559** |
| ai_squad_mm | 0,966 | — | 0,976 |
| anim1000 | 4,738 | 4,864 | 4,804 |
| debug_viewer (casa) | 1,224 | 1,231 | **1,219** |

- Próximo ganho possível: compactar os draws indiretos (`DrawIndirectCount`), para que malhas fora da cena não custem.
- `crowd` "the agents should close in": a 0.14 falha igual (distância média 1511 cm, contra 1490 nas rodadas que
  passam). O limite está colado no resultado e varia com a carga da CPU; não vem dos merges.

## Cabelo piscando: sombras por raio pelo alfa (2026-11-03, relatório #48)
Relatório: a luz na Delight (importada do Tlender) piscava jogando. Medido com a câmera fixa, nível limpo (chão, sol,
céu), Delight a 2 m:
- Parada (editor), a luz dela não vai e volta: 0,01–0,02% da vista. Jogando, a animação dela é "nervosa" (cabeça e
  olhos dão trancos de ~6 cm por quadro, iguais no Tlender), então a medida comum de flicker mede a animação: GI
  desligada, sem RT ou com a animação antiga dão os mesmos ~0,9%. Com a animação 20× mais lenta (experimento) o resto
  dependia só do RT.
- **Causa**: os raios de sombra eram opacos (`gl_RayFlagsOpaqueEXT`) e ignoravam o alfa do Masked: cada card de cabelo
  projetava a sombra do quadrilátero inteiro — blocos claros e escuros de borda reta no cabelo (no mapa de sombra, que
  corta pelo alfa, não), que saltavam quando a cabeça mexia. Agora as instâncias das malhas Masked entram nos dois TLAS
  como não opacas (`FORCE_NO_OPAQUE`: `tlas_fill.comp` pela `MESH_INFO_MASKED`, o dinâmico por `R.mesh_masked`) e o raio
  de visibilidade do `mesh.frag` (`rt_alpha_blocks`, `rt.glsl`) lê o alfa assado no ponto do candidato (índices,
  baricêntricas, UV × tiling) — a sombra sai por fio. Os opacos continuam sem custo extra; os shaders sem materiais
  (sondas, cache, path tracer) confirmam o candidato (como antes).
- Só nas cenas com malha Masked: o `mesh.frag` com RT ganhou variantes `-DRT_ALPHA` (`mesh_*_a.frag.inc`, CMake), que
  `build_msaa_pipelines` escolhe quando `R.any_masked` (a primeira malha Masked desenhada refaz as pipelines MSAA uma
  vez) e `GI_RT_MASKED` liga o caminho. Com o teste de alfa no mesmo shader (mesmo desligado por constante de
  especialização) a segunda ray query tomava registradores: passe principal +0,45 ms e flicker 0,077 → 0,108% na
  `house_movable`; com as variantes, sem Masked o shader é o de antes (flicker 0,077 × 0,076%, `play_flicker` igual).
- Custo na cena da Delight (A/B intercalado com o HEAD, pré-passe ligado): passe principal 2,53 × 2,54 ms, 246–253 ×
  240 fps — no ruído (o pré-passe deixa uma camada de cabelo por pixel para sombrear).
- Testados e deixados de fora (sem benefício mensurável aqui, mexem no sombreamento de tudo): a normal da face de trás
  por `gl_FrontFacing` em vez do sinal de `n·v`, e o corte Masked com MSAA como máscara de amostras (alfa afiado pelo
  `fwidth`). Ficam como candidatos se o cabelo cintilar de longe.
- Medida nova no `--bench`: "Flicker back and forth" — a parte da vista que muda > 2% e volta no quadro seguinte
  (o movimento suave não conta). `mat_common.glsl` entrou nas dependências dos shaders (antes mudar ele não recompilava).
- Resto conhecido: as partes de personagem não têm quadro no cache de superfície (seria um mosaico: um texel por
  triângulo), são iluminadas pelas sondas, e as sondas têm ruído por quadro (o chão só com sondas: 2,6% da vista na
  visão GI); com a personagem parada isso dá deriva lenta, não vai e volta.

## Sombras pelo alfa com muitas camadas de cards (2026-11-05, relatório #113)
Relato: "cena com 4 Delights rodando a 32 fps; também flickando as luzes". Projeto do usuário (MyProject, 4 Delights
importadas do Tlender numa fila, cabelo e vestido rasgado Masked, uma PointLight com Source Radius 39,5 cm a 80 cm
acima da cabeça de uma delas), Épico, 2560x1417, MSAA 8x: **34 fps**, passe main mínimo 21,7 ms.
- **A causa.** A luz de área tem 8 raios por pixel no Épico e, com ela baixa e as Delights alinhadas, os raios de boa
  parte da tela atravessam cabelo e vestido. Cada triângulo Masked que o raio cruza volta ao shader para o teste de alfa
  (`rt_alpha_blocks`). Medido no passe main: sem raios de sombra 5,0 ms; com raios opacos 8,9 ms; com o alfa 21,7 ms.
  Só o cabelo Masked (vestido opaco, experimento) já dava 18 ms.
- **Testado e deixado de fora:**
  - orçamento de testes de alfa por pixel: com 4, 12,5 ms, mas o chão fica granulado (poucos raios onde o orçamento acaba);
  - raios proporcionais à cobertura MSAA da invocação (`gl_SampleMaskIn`): sem efeito, porque os cards cobrem o pixel
    inteiro e cada pixel tem uma invocação;
  - um raio só contra o opaco (`CullNoOpaque`) e depois só contra o Masked (`CullOpaque`): 37 ms (as flags de cull
    pioram a travessia);
  - `NO_DUPLICATE_ANY_HIT` nas BLAS: sem efeito;
  - alfa lido no mip 2: -10%.
- **Na 0.19** (desfeito): os raios pares primeiro e parar em 4 se algum cruzou card Masked. Na cena de teste não se
  via diferença, mas no vídeo do usuário (a mesma cena, a luz em outro lugar) o chão inteiro ficou granulado: os raios
  do chão para a luz cruzam os vestidos. Parar quando os 4 concordam também granula (pixels com 0 ou 1 exatos ao lado
  de pixels fracionários). O `light_visibility` voltou ao da 0.18; a correção é a seção seguinte.
- **O "piscar"** era o próprio pontilhado: a espiral de cada pixel gira pela posição na tela (`ign`), então o padrão
  "nada" quando a câmera anda. No editor parado, a medida dá 0,00%.

## Sombras de luz de área filtradas: o shadow split (2026-11-05, vídeo do relatório #113)
Vídeo do usuário (jogando, Épico, 41-53 fps): a sombra das Delights no chão é um pontilhado regular que se mexe com a
câmera. Com 4-8 raios por pixel e sem filtro, a penumbra de uma luz de área sempre sai assim. A ideia é a da Unreal:
traçar poucos raios e filtrar a visibilidade.
- **mesh.frag** (variantes `RE_RT`, `GI_SHADOW_SPLIT`): a primeira luz de área de cada pixel não entra em `o_color`.
  Ela sai numa segunda saída (`o_split`): a luz sem sombra (pré-exposta, com a transmitância do ar e da neblina) e a
  luminância dela vezes a visibilidade crua. Essa luz usa 1/4 dos raios (mínimo 2; Épico 2, Cinemático 4). As outras
  luzes não mudam.
  - Com MSAA, o resolve tira a média dos dois valores. Então `a / luminância` é a visibilidade das amostras pesada
    pela luz de cada uma, e as bordas ficam corretas sem regra especial.
  - Os 2 valores ficam empacotados em meio float (`packHalf2x16`) e `o_split` só é escrita no fim. Uma saída escrita
    no começo do shader fica viva no shader inteiro e fazia o `mesh_rt` (128 registradores) fazer spill.
- **shadow_split.comp**, depois da passada principal, é um bilateral separável de 13 taps (gaussiana, peso pela
  profundidade relativa, mais tolerante longe do centro para chão rasante).
  - O passe 0 (x) grava as médias de `a` e da luminância (`split_sum`, RG16F).
  - O passe 1 (y) soma ao HDR `rgb * (média a / média luminância)`.
  - A profundidade de uma amostra é a resolvida (MIN). Com o filtro ligado, a passada principal resolve sempre,
    também com o pré-passe de oclusão: o resolve dele só tem a lista early.
- A segunda saída fica sem imagem quando o filtro está desligado (`VK_EXT_dynamic_rendering_unused_attachments`,
  ligada no vkctx.c). Sem essa extensão, a passada principal continua com uma saída e o filtro não existe (o caminho
  da 0.18).
- **Só liga** com RT, vista Iluminado ou Só Iluminação, `independentBlend` (ligado no vkctx.c) e alguma luz de área
  projetando sombra no mundo (`lights_area_count`, contado pelo `lights.c` a cada envio de luz). Sem luz de área, a
  segunda saída não tem imagem (as escritas são descartadas) e o custo é igual ao da 0.19. `REALENGINE_NO_SHADOW_FILTER=1`
  desliga o filtro (comparações).
- **Medido** (RTX 3060, 2560x1417, mínimo da GPU, mesmo executável e mesma Content):
  - **As Delights no Épico** (a cena do vídeo): o passe main ficou em ~11 ms. A 0.18 dava ~20 ms, a 0.19 10–14 ms
    com o chão granulado. O chão ficou liso e os personagens iguais à 0.18.
  - **A `house` no Épico** (sem luz de área): 1,82 ms, igual à 0.19.
  - **Custo do filtro com luz de área:** a segunda saída MSAA 8x e o compute custam ~0,5 ms a 1440p.
- Registradores: `mesh_rt` 128 com 16 bytes de spill (antes 0) por causa da segunda saída. Medido na `house`, isso não
  muda o passe (1,82 × 1,84 ms). As `_a` ficaram com 48 bytes (antes 16). O teste `shader_spill` (`--no-spill`) passa
  a aceitar até 32 bytes somados (16 no `mesh_rt`, 16 no `mesh_rt_t`).
- Teste `shadow_filter`: a `house` com a spot e a point viradas esferas de 30 cm (`--source-radius 30`), com
  validação e flicker.

## Pipelines Masked em segundo plano (2026-11-05, travamento da 0.18)
A primeira malha Masked desenhada refazia todas as pipelines MSAA na thread principal, para usar as variantes com
teste de alfa. Com o cache de pipelines frio (a primeira abertura depois de uma atualização: os shaders mudaram), o
driver ficou mais de 8 s compilando. O vigia gravou `RealEngine_travamento_2026-11-04` com a pilha em
`build_msaa_pipelines`, ao abrir o RPG (folhagem Masked).
- Agora `rt_alpha_job_start` (render.c) compila numa thread só as 6 pipelines de RT com alfa (`build_rt_pipelines`).
  Enquanto isso, os raios de sombra seguem opacos (`GI_RT_MASKED` desligado, como antes de haver Masked).
  `rt_alpha_job_poll`, no quadro, troca o conjunto quando ele fica pronto (um `vkDeviceWaitIdle`). Se o MSAA mudou
  no meio, descarta o resultado. O desligamento espera a thread batendo no vigia.
- Medido (o projeto da Delight, `__GL_SHADER_DISK_CACHE=0` e um cache de pipelines novo, vigia em 2 s): a 0.18 parou
  8+ s; agora nenhum aviso, 81 fps enquanto compila, e a troca acontece em 33 s (a thread disputa a CPU com o editor
  sem limite de fps). Com o cache quente, a troca leva 206 ms.
- Prioridade normal: abaixo dela, a thread não terminou em 25 s.

## Serrilhado: o corte Masked como cobertura e o resolve pelo brilho (2026-11-05)
Pedido do usuário: "o cabelo fica bem serrilhado, e várias coisas". No Épico (MSAA 8x), as quinas das malhas já saíam
suaves. Duas coisas não saíam:
- **O corte Masked era por pixel.** O pré-passe (`masked.frag`) descartava o pixel inteiro pelo alfa, então o MSAA não
  ajudava: fios de cabelo e pontas rasgadas do vestido saíam em degraus de 1 pixel. Agora o alfa vira cobertura das
  amostras (`gl_SampleMask`, `alpha_coverage` no `mat_common.glsl`; o alpha-to-coverage afiado). A borda tem 1 pixel
  de largura (`(a - clip) / fwidth(a)`), a fração de amostras é mantida, e uma rotação por pixel e instância evita
  que dois cards empilhados fiquem com as mesmas amostras.
  - A passada principal (EQUAL) sombreia só as amostras que o pré-passe manteve, e o resolve suaviza.
  - O caminho sem pré-passe (`mesh.frag` RE_MASKED) faz o mesmo.
  - A sombra do sol (passe 1) continua com o corte simples.
  - Sem MSAA, é o corte de antes no valor do clip.
  - `sky_params.z` leva as amostras.
- **O resolve tirava a média linear do HDR.** Um pixel metade fio, metade parede clara ficava quase tão claro quanto
  a parede, e as bordas contra fundos claros (parede, céu) pareciam serrilhadas. Agora o resolve é ponderado pelo
  brilho (Karis): `mesh.frag` e `sky.frag` escrevem `aa_encode(c) = (c·w, w)` com `w = 1/(1 + max(c))` (no alfa, que
  o HDR não usava; `GI_AA_WEIGHTED`, vistas Iluminado e Só Iluminação com MSAA). O resolve tira a média dos dois, e o
  passe 2 do `shadow_split.comp` divide (`rgb / a`) antes de tudo o que lê o HDR.
  - A primeira versão guardava a curva `c/(1+max)` na cor e a invertia depois. Em meio float, a inversão amplificava o
    arredondamento 2–6×, e o ruído de cada quadro da GI virava degraus: o flicker da `house_movable` foi de 0,066% para
    0,127%. Com o peso no alfa, a divisão é exata (0,089% × 0,083% do resolve linear).
  - A grade (blend pré-multiplicado) continua como era: sobre `(c·w, w)` o blend deixa uma mistura que a divisão lê
    como as linhas com peso 1.
  - `REALENGINE_PLAIN_RESOLVE=1` volta ao resolve linear (comparações).
- O antialias especular (Kaplanyan–Hoffman, a rugosidade alargada pela variação da normal) já existia.
- **Medido** (RTX 3060, 1440p, mínimo da GPU, mesma pasta e mesma Content):
  - **Delights no Épico:** custo igual.
  - **RPG Médio** (folhagem Masked): passe main 3,09 × 2,97 ms (+4%: as camadas que agora se misturam na borda
    sombreiam mais amostras), 176–183 × 183–188 fps.
  - **`crowd20000`:** a diferença fica abaixo do ruído (5,1–5,6 ms nas três variantes).
  - **Brilho médio** igual ao do resolve linear (a `house`: 112,36 × 112,44).
  - **Validação** sem avisos, com e sem o pré-passe.
- Com o Blender aberto na mesma máquina, os testes de fps (`crowd_20000`, `demo_rpg`) oscilaram abaixo do limite:
  a 0.19 deu 84–85 fps no `crowd20000` no mesmo momento.

## Malhas pesadas: fases 0 e 1 (2026-11-06)
Pedido: um modelo de mais de 2 M triângulos, com material Masked e texturas pesadas, sem cair abaixo de 100 fps. A
pesquisa e o plano em fases (clusters estilo Nanite, Masked rápido, modo Blend, compressão BC) estão no chat de
2026-11-06. Estas são as fases 0 e 1.
- **Antes:**
  - **O modelo nem carregava.** Os importadores gravavam um vértice por canto de triângulo (3 × 32 B por triângulo,
    para o UV2 de lightmap ter uma célula por triângulo), e `load_imported` descartava, sem aviso, acima de 4 M
    vértices (~1,33 M triângulos).
  - **Sem preparo da malha.** Não havia LOD nas importadas nem otimização de ordem.
  - **Draws vazios.** Um comando indireto por malha × LOD, mesmo vazio.
  - **Limite de 255 malhas.**
- **Fase 0: a cena `heavy`** (`heavy_test.c`, `--scene heavy`, só dentro de um projeto). Um modelo de 2,14 M
  triângulos: uma rocha deslocada de 1,54 M (opaca, ruído 4096 com bump) e 300 k cards Masked em volta (600 k
  triângulos, voronoi 4096 de dois lados, camadas sobre camadas como cabelo ou arbusto). Nove cópias até 60 m e uma
  PointLight de 30 cm. Teste `heavy_mesh` (Épico sem RT, validação, ≥ 50 fps; projeto próprio, nunca a Content da
  build).
- **Fase 1** (`mesh_build.c`, com a `meshoptimizer` 1.3 em `external/`, MIT):
  - **`.rmesh` v2**: indexado, tabela de LODs (até 4, com o erro em cm), flags. A v1 continua abrindo: é processada
    na carga e o resultado fica no cache (`%LOCALAPPDATA%\RealEngine\meshes`, pela hash do conteúdo). Os
    arquivos do projeto não são reescritos, para uma versão antiga continuar abrindo. 124 malhas da Delight: 1,4 s
    na 1ª abertura, ~0,2 s depois.
  - **Solda**: acima de 65 536 triângulos os cantos iguais viram um vértice (`RMESH_NO_ATLAS`: sem UV2, a malha não
    ganha lightmap nem quadro no cache de superfície e é iluminada pelas sondas, como as partes dos personagens). Um
    quadro de milhões de células daria menos de um texel por triângulo. Até esse tamanho, o UV2 por triângulo continua
    como antes.
  - **Ordem**: cache de vértices, overdraw (1,05) e busca de vértices (`meshopt_optimizeVertexCache/Overdraw/Fetch`).
  - **LODs** de toda importada com 1024+ triângulos:
    - Superfície: simplificação com normal e UV pesados e a borda travada, 1/4 por nível
      (`meshopt_simplifyWithAttributes`). Com UV2, os LODs ganham vértices próprios com uv2 −1 (o `mesh.frag` lê
      sondas nesses).
    - Muitas peças pequenas (cards, mais de 1/16 de peças por triângulo): desbaste. Cada nível guarda 1 de 4 peças
      (hash aninhado), cada uma 2× maior em torno do centro para cobrir a mesma tela; o erro é o raio da peça / 4.
    - Escolha (`cull.comp`, `MESH_INFO_ERROR_LOD`): o LOD mais grosso cujo erro na tela fica abaixo de 1 pixel. As
      formas básicas continuam pela regra do raio.
  - **Draws compactados**: o passe 1/4 do `cull.comp` empacota, por sequência de malhas da mesma classe, só os
    comandos não vazios e a contagem; `draw_runs` usa `vkCmdDrawIndexedIndirectCount`.
  - **Limite de 1023 malhas** (era 255): os contadores do cull num array só (as bases substituem as contagens no
    lugar: 32 KB de memória compartilhada).
  - **Oclusão automática com Masked**: o pré-passe fica sempre ligado. O quadro de teste sem ele levava 562 ms com os
    cards (uma parada a cada 20 s; no cabelo da Delight, 83 ms).
- **Medido** (RTX 3060, 2560x1417):

| Cena | Antes | Agora |
|---|---|---|
| Rocha 1,54 M (LODs) | — | 385 k / 96 k / 24 k (2,1 s de processamento) |
| Cards 600 k (LODs) | sem LOD | 150 k / 38 k / 9,6 k (0,16 s) |
| `heavy`, triângulos desenhados | 5,7 M | 4,3 M |
| `heavy` Médio | 62 fps | 68 fps |
| `heavy` Épico sem RT | 47 fps, pior quadro 258 ms | 63 fps, pior 17–19 ms |
| Delights (Épico), draws | 1550 comandos | 80 |
| `house` Épico na build (89 malhas registradas), passe main | 2,41 ms | 1,81 ms |

- **O que falta para 100 fps no Épico com o modelo pesado:**
  - **RT ligado:** 12 fps, mínimo da GPU 43 ms. Quase tudo são as sombras por raio da luz de área atravessando os
    cards (como no #113): é a fase 3 (Masked rápido).
  - **Sem RT:** a geometria e o Masked de 4,3 M triângulos em MSAA 8x (passe main 14,7 ms). Pedem a fase 2 (clusters
    com culling por cluster e LOD contínuo dentro de um mesmo objeto: o herói perto da câmera desenha a rocha inteira
    em LOD 1).

## Malhas pesadas: fase 2, clusters (2026-11-06)
Cada objeto Masked grande é desenhado por clusters, testados um a um contra a tela e o Hi-Z. O que as malhas opacas
escondem, ou o que sai da vista, deixa de custar.
- **Erro do LOD por escalabilidade** (`gi.lod_error_px`, `pad_cull1`): Épico 1 px, Alto 2, Médio 4, Baixo 8, Cinemático
  0,5.
- **Clusters** (`mesh_build.c`, `mesh_clusters` / `mesh_built_clusters`):
  - Montagem: malhas importadas com 16 k triângulos ou mais, por LOD. São meshlets da meshoptimizer, de até 128
    vértices e 128 triângulos, cada um com a esfera de contorno.
  - Ordem: os triângulos são reordenados cluster por cluster, os de fora primeiro (distância ao meio da malha). Na
    ordem dos próprios meshlets, o passe main da cena `heavy` ia de 14 para 27 ms, porque o pré-passe com alfa perdia a
    rejeição das camadas internas. Com a ordem de fora para dentro, 17 ms.
  - **Ordens de vista:** mais 6 ordens por cluster (`cl_perm`), de frente para trás ao longo de ±X, ±Y e ±Z. O cull
    escolhe por instância a do eixo mais próximo da direção câmera → objeto. O pré-passe dos cards caiu de 8,3 para
    7,8 ms.
  - **Cache:** o resultado vai para o cache (`meshes/<hash>_cl1.bin`, pela hash do conteúdo). Na cena `heavy`, a carga
    foi de 2,4 s para 0,12 s.
- **Na GPU** (`cull.comp`):
  - **Passe 0:** uma instância Masked com clusters no LOD escolhido não entra na lista antecipada (`CLUSTERED`). Ela
    ganha um draw próprio e reserva o índice inteiro do LOD em `cl_idx`, além dos blocos de 64 clusters. Sem espaço,
    `CL_MAX_ENTRIES` 4096 e 16 M índices: desenhada inteira, como antes. A sombra segue pela instância.
  - **Passe 6 (depois do Hi-Z das malhas desenhadas inteiras):** por bloco, quais clusters estão na tela e não estão
    atrás da profundidade (máscara e contagem).
  - **Passe 7:** a soma dos blocos anteriores dá onde cada bloco começa, e os índices dos vistos são copiados **na
    ordem**. Com atômicos, a ordem mudava a cada quadro e o main oscilava de 14 a 30 ms.
  - Os dois passes vão por `vkCmdDispatchIndirect`: x = instâncias, y = blocos.
- **Desenho** (`render.c`, `cluster_record` / `cluster_draws`):
  - Um 2º pré-passe (profundidade LOAD, com o resolve MSAA) com `push.pass` 6, usando o buffer de índices
    `cl_idx`.
  - No passe main, os mesmos draws com o pipeline Masked EQUAL.
  - `mesh.vert` lê a slot pelo `gl_InstanceIndex` do draw.
- **Medido** (RTX 3060, 2560x1417, Épico sem RT, mínimo da GPU; a média variava com o Blender aberto):

| Câmera da cena `heavy` | Sem clusters | Com clusters |
|---|---|---|
| padrão (-380,-160,170, yaw 20) | 16,2 ms | 14,2 ms |
| perto da rocha (-200,-80,90) | 16,1 ms | 12,3 ms |
| de lado (yaw 60) | 12,4 ms | 10,9 ms |

  Imagem igual à sem clusters (0,011% dos pixels com diferença acima de 16/255, pela ordem dos cards).
- **O que custa agora** (câmera padrão): os cards somam ~6 ms no pré-passe com alfa e ~6 ms no main (EQUAL: em MSAA
  8x com alpha-to-coverage, um pixel de borda é sombreado por várias camadas; triângulos de 1 px sombreiam quads
  quase vazios). O resto da cena custa 2,6 ms.
- **Próximo, a fase 3 (Masked rápido):**
  - classificar os triângulos opacos e os recortados;
  - mips de alfa que preservam a cobertura;
  - menos sombreamento por pixel de borda;
  - o custo dos raios com alfa.

## Malhas pesadas: fase 3, Masked rápido (2026-11-06)
- **Onde estava o custo** dos cards na cena `heavy` (Épico, mínimo da GPU, medido trocando uma coisa por vez):
  - **Sombreamento:** trocar o shading principal dos cards por uma cor fixa economiza só 0,6 ms. Não é ele.
  - **Triângulos:** o limiar de LOD de 1 para 8 px economiza 3 ms. Não é a maior parte.
  - **MSAA 8×:** era a maior parte. 14,4 ms em 8×, 9,6 ms em 4×, 8,0 ms em 2×. Os cards são borda em toda parte (o
    alpha-to-coverage dá cobertura parcial em cada pixel), a compressão das amostras não funciona e cada pixel paga 8.
  - **Raios com alfa (com RT):** 16 ms. 38,4 ms com o teste de alfa contra 22,3 ms tratando como opaco.
- **Medida confiável:** o `--bench` espera as pipelines de raio com alfa, que são feitas numa thread. Antes, o mínimo
  pegava quadros com raios ainda opacos e variava de 22 a 38 ms (`render_pipelines_pending`).
- **Mudanças:**
  - **Épico em MSAA 4×**, como o renderer forward da Unreal. O Cinemático fica em 8×.
  - **Híbrido do sol só em receptores opacos:** nos próprios cards Masked a VSM basta (a sombra ali é um pontilhado de
    folhas). Na `heavy`, o híbrido custava 6,7 ms; agora 0,4 ms.
  - **Raios leem o alfa num mip de até 512 texels:** o mip 0 de uma textura 4096 estourava o cache candidato após
    candidato. Ganho de ~1 ms.
  - **Mips que preservam a cobertura do recorte** (Castaño 2010, a do NVTT):
    - O problema: o blit linear dos mips mexe na cobertura. Fios finos de cabelo caem abaixo do valor de corte e somem
      com a distância (e nas sombras, que leem mips mais grossos); manchas redondas engordam.
    - Depois do bake, `alpha_hist.comp` tira o histograma do alfa de cada mip. A escala é a que devolve a cobertura do
      mip 0; quando o valor de corte já cai no intervalo certo, a escala é 1.
    - A escala vai **gravada no alfa do próprio mip** (uma view e um descriptor set por mip), sem custo por pixel. A
      versão com a escala lida no shader custava 1,5 ms do pré-passe.
    - Nos cards de voronoi, os mips médios ficam com 0,68–0,72. Custa 0,85 ms na `heavy` porque as bordas, agora do
      tamanho certo, têm cobertura parcial.
    - `REALENGINE_NO_ALPHA_COVERAGE=1` compara.
    - Não refeito quando o streaming aumenta uma textura: esses mips ficam com o blit simples.
  - **Descartado:** opacity micromaps (`VK_EXT_opacity_micromap` existe na RTX 3060). Não achei confirmação de
    aceleração em hardware fora da série 40, e a implementação é grande.
- **Medido** (RTX 3060, 1440p, mínimo da GPU; o seu MyProject no Play):

| Cena | Antes | Depois |
|---|---|---|
| `heavy` Épico sem RT | 14,4 ms | 10,1 ms (~99 fps) |
| `heavy` Épico com RT | 38,4 ms | 26,4 ms |
| MyProject | 11,4 ms | 10,6 ms |

## Translúcidos: Blend Mode Translucent com OIT (2026-11-06, fase 4)
Não existia material translúcido (só Opaque e Masked). Agora o Blend Mode **Translucent** usa o alfa assado como
opacidade. A técnica é a **weighted blended OIT** (McGuire e Bavoil 2013), a mais rápida das que dispensam ordenar.
- **Material:** `MATF_TRANSLUCENT`, salvo como `"blend_mode":"Translucent"`, com opção no editor.
  - A instância ganha `INST_TRANSLUCENT`: não projeta sombra e fica fora do ray tracing (máscara 0 nos dois TLAS).
  - A malha ganha `MESH_INFO_TRANSLUCENT`: as sequências de draw do `cull.comp` se separam também por ela, e o pré-passe
    e o passe principal a pulam. Nunca vira cluster.
- **Passe OIT** (`render.c`, depois do filtro de sombra):
  - Desenha as listas antecipada e tardia só das malhas translúcidas, numa amostra, com teste de profundidade contra a
    profundidade resolvida (forçada nesse quadro) e sem gravar nela.
  - Duas saídas, reaproveitando as imagens do filtro de sombra (livres nesse ponto):
    - acumulada: cor × opacidade × peso, somada;
    - revelação: produto de (1 − opacidade).
  - O peso é a eq. 10 do artigo, em metros, limitado a 30 para o meio float aguentar dezenas de camadas.
  - `shadow_split.comp` passe 3 compõe: `hdr × revelação + média × (1 − revelação)`.
  - `mesh.frag` `RE_TRANSLUCENT`, com e sem RT (sombras e reflexos no vidro): sem o split de luz de área.
- **Cena `glass` e teste `translucent`:** 3 vidros grandes sobrepostos na frente de uma esfera e de um cubo, mais 400
  vidros pequenos em camadas. Validação sem erro, as cores se misturam na ordem certa.
- **Medido** (Épico, 1440p, mínimo da GPU): o passe custa 2,2 ms com os 403 vidros (4,8 contra 2,6 ms com
  `REALENGINE_NO_OIT=1`). Igual com ou sem raios: o custo é o preenchimento de ~20 camadas, não a luz.

## Otimização geral: fase 0 (medida) e fase 1 (upscaling temporal) (2026-11-06)
Pedido: otimizar a engine inteira para cenas muito pesadas com qualidade de cinema acima de 100 fps. Plano em fases:
0 medida, 1 upscaling temporal, 2 muitas luzes, 3 LOD de clusters em DAG, 4 GPU-driven em tudo, 5 virtual texturing.
- **Fase 0, a medida de referência:** `sh tools/bench_ref.sh <rótulo>` roda heavy (Épico, RT), casa assombrada (Play),
  RPG (Play) e house, em segundo plano, e imprime fps, mínimo da GPU e o custo de cada passe.
- **Fase 1, Temporal Super Resolution** (`shaders/tsr.comp`, como o TSR da Unreal):
  - **Render menor:** a cena é desenhada numa fração da saída (`gi.tsr_scale`: Baixo 50%, Médio 60%, Alto 67%, Épico
    75%, Cinemático 100%).
  - **Jitter:** a projeção anda um subpixel por quadro (Halton 2,3, 8 fases). As matrizes do histórico ficam sem jitter.
  - **Reconstrução** no tamanho da saída:
    - gaussiana dos 3×3 pontos do render pela distância;
    - reprojeção pela câmera (profundidade → mundo → quadro anterior) mais a velocidade própria dos atores;
    - histórico Catmull-Rom;
    - clip de variância em YCoCg: 1,25σ em movimento, 2,5σ parado;
    - cores em espaço tonemapped.
  - **Peso da amostra nova:**
    - em repouso, 0,010–0,025; em movimento, 0,03–0,08, mais a velocidade;
    - nunca menos que 1/(quadros acumulados + 1). O alfa do histórico conta os quadros, e o clip o reduz: um pixel novo
      converge em poucos quadros.
  - **Histórico em RGBA16 UNORM:** a cor é gravada como c/(1+max(c)). Em half float, os passos pequenos do repouso ficavam
    abaixo da precisão e o pixel pulava 1 ulp a cada quadro (cintilação 0,159 → 0,114% só com 32 bits).
  - **Exposição:** o histórico é reescalado de uma exposição para a outra (`e.exposure / e.pre_exposure`). Sem isso, cada
    passo da exposição automática pulsava a imagem inteira (histórico congelado: 0,157% → 0,047%, igual a sem TSR).
  - **Afiamento** na composição, preso ao intervalo da cruz (sem halo): 0,6 a 50%, 0,25 a 100%.
  - **Passes:** os alvos da cena ficam no tamanho do render, e o HDR, a máscara e o bloom no da saída. A máscara de
    seleção usa a matriz sem jitter.
- **MSAA:** sai do Baixo ao Épico, porque o TSR faz o antisserrilhado (como na Unreal). No heavy, TSR sobre 4x dava 21,5
  ms contra 13,9 sem MSAA. O Cinemático mantém 8x com TSR a 100%.
- **Variáveis de ambiente:** `REALENGINE_NO_TSR=1`, `REALENGINE_TSR_SCALE=<0.25-1>` e `REALENGINE_MSAA=<n>` (medidas).
- **Custo do passe:** 0,47 ms a 1440p (house, TSR 100% contra sem TSR, os dois sem MSAA).
- **Medido** (Épico, 2560×1417, RTX 3060, `bench_ref.sh`):

| Cena | Antes (MSAA 4x) | Depois (TSR 75%) |
|---|---|---|
| heavy | 31 fps, GPU 27,8 ms | **65 fps**, 13,1 ms |
| casa assombrada | 93 fps, 9,27 ms | **147 fps**, 5,91 ms |
| RPG | 114 fps, 7,56 ms | **158 fps**, 5,61 ms |
| house | 313 fps, 2,62 ms | **364 fps**, 2,29 ms |

  **Qualidade:** igual ao nativo com 4x, com bordas um pouco mais suaves e sem rastro na figura andando da casa.
  **Cintilação** com a câmera parada: house 0,047% sem TSR → 0,072% com TSR, dentro dos limites. A suíte passa inteira
  (81/81).
- **Correções (2026-11-07, antes da 0.22):**
  - **Corte de câmera:** o corte (histórico descartado) comparava os 16 termos da matriz com 0,5, inclusive os de
    translação, que estão em cm. Com isso, qualquer câmera andando mais de ~0,5 cm por quadro virava corte: um personagem
    andando ficava sem histórico, com os quadros com jitter crus. Agora a rotação e a lente vêm dos termos sem unidade, e
    o movimento vem da posição da câmera (corte acima de 3 m num quadro). house_walk: 0,095 → 0,081%.
  - **Picos de 3-10% no `play_flicker`** (1 em ~10 execuções, também na main): o teste criava um chão em z 0 por cima do
    Floor do nível. Os dois brigavam pela profundidade, e o jitter trocava o vencedor a cada quadro. O chão do teste
    desceu 1 cm: 50 execuções sem pico, base 0,077 → 0,056%. (Como na Unreal, superfícies coplanares cintilam com
    TSR: o jitter mostra o z-fighting.)
  - **Multidões:** sem velocidade, os agentes deixavam rastro (notas-dev/multidao.md).

## LOD de clusters em DAG (2026-11-07, otimização geral fase 3)
Antes, um objeto inteiro trocava de LOD de uma vez (4 LODs discretos, 1/4 cada). Uma rocha perto da câmera ia toda no
LOD 0, inclusive o lado de trás e o que saía da tela. Agora as malhas opacas grandes têm um DAG de clusters como o Nanite:
cada pedaço do objeto escolhe o próprio nível.
- **Construção** (`mesh_clod.cpp`, sobre o `clusterlod.h` da meshoptimizer 1.3):
  - Clusters de até 128 triângulos sobre o LOD 0.
  - Grupos de ~16 clusters são simplificados juntos, com a borda do grupo travada (sem rachaduras entre níveis), e a
    simplificação é refeita em clusters, nível após nível.
  - Simplificação permissiva com normal e UV pesados e as costuras de UV protegidas.
  - Os índices do DAG apontam para os mesmos vértices do LOD 0 e vêm depois dos índices da malha.
  - Cache: `meshes/<hash>_dag1.bin`.
  - Rocha de 1,54 M triângulos: 25 mil clusters em 15 níveis (3,1 M triângulos no total), 3,1 s na primeira vez.
- **Quem tem:** malhas com 16 k triângulos ou mais e que não sejam de peças soltas (mais vértices que triângulos, como
  os cards). Um DAG não simplifica cards, e os LODs desbastados deles continuam.
- **Escolha** (`cull.comp`):
  - **Passe 0:** uma instância opaca com DAG entra como clusters no LOD 0, ou no LOD 1 quando cruza a borda da vista.
    - No meio da distância o erro do DAG, acumulado pelos níveis, é mais cauteloso que o de um LOD inteiro: desenhava
      mais que o LOD 1 (+0,3 ms no `heavy`).
    - O desenho vai no segundo array de draws (`CL_DRAWS2_AT`), pelos pipelines opacos.
  - **Passe 6:** um cluster é desenhado se o próprio erro na tela fica ≤ o limite (`gi.lod_error_px`, Épico 1 px) e o do
    pai fica acima.
    - Os dois lados de um nível usam a mesma esfera e o mesmo erro do grupo: sai exatamente um corte, sem buraco nem
      sobreposição.
    - O cluster também precisa estar dentro da vista.
    - **Ordem:** os DAGs são escolhidos antes do pré-passe (`cluster_record` grupo 0, sem Hi-Z) e desenhados nele, porque
      são oclusores. Os Masked continuam depois do Hi-Z (grupo 1).
- **Sombras por raio:** o BLAS continua no LOD 0. O viés do raio num cluster do DAG é o erro máximo do corte na distância
  do vértice (limite em px × cm por pixel × 1,25), em `mesh.vert`. Sem isso, faixas pretas apareciam nas bordas.
- **Medido sem ganho e descartado:** descartar os clusters virados para trás pelo cone de normais (4,73 × 4,68 ms; o
  rasterizador já descarta essas faces barato).
- **Cena `rocks`** (`--scene rocks`, num projeto) e teste `cluster_lod`: 100 rochas de 1,54 M triângulos em grade, com a
  câmera baixa entre elas. O teste usa validação e acusa FAIL se a rocha não tiver DAG. `REALENGINE_NO_DAG=1` desliga o
  DAG para comparar.
- **Medido** (Épico, 2560×1417, RTX 3060, mínimo da GPU, rodadas com a GPU livre):

| Cena | Sem DAG | Com DAG |
|---|---|---|
| `rocks` | 5,52 ms (main 4,26) | **4,75 ms** (main 3,41) |
| `heavy` padrão / perto (rocha atrás dos cards) | 12,7–13,3 / 19,6–22,4 ms | 13,2–13,5 / 20,3–21,7 ms (no ruído) |
| casa assombrada (2 props com DAG) | 6,32 ms | 6,37 ms |

  Imagem igual à sem DAG: diferença média de 0,31/255, e 0,047% dos pixels acima de 16/255.
- **Próximo:** oclusão em duas fases (Hi-Z do quadro anterior antes do pré-passe, os rejeitados testados de novo depois)
  e uma BVH sobre os grupos (`clodBuildHierarchy`), para não testar todos os clusters de cada instância. Isso abre malhas
  de dezenas de milhões de triângulos.

### Oclusão do DAG em duas fases e correções (2026-11-07)
- **Duas fases** (como o Nanite):
  - **Fase 1** (antes do pré-passe): cada cluster do DAG é testado contra o Hi-Z do quadro anterior, projetado com a
    matriz do quadro anterior (`occluded_prev`, `tsr_prev_vp`). Isso exige que o Hi-Z seja daquela vista: a matriz de
    quem o construiu é guardada, e uma 2ª viewport não o reaproveita.
  - **Fase 2** (depois do Hi-Z novo): os clusters que a fase 1 não desenhou e que agora aparecem vão para um terceiro
    array de draws (`CL_DRAWS3_AT`), no 2º pré-passe e no passe principal.
- **Adaptativa:** a oclusão só liga quando os DAGs desenham muitos triângulos. Liga acima de 500 mil por quadro e
  desliga abaixo de 250 mil, contando o quadro anterior (lido de volta).
  - Na `rocks` ela tira 30% dos triângulos do DAG (78 → 55 mil), sem efeito no tempo, e a 2ª fase custa ~0,1 ms.
  - `REALENGINE_DAG_OCCLUSION=1` força ligada; `REALENGINE_NO_DAG_OCCLUSION=1` força desligada.
- **Esfera exata de cada cluster** (`meshopt_computeClusterBounds`): o `clusterlod` dá a um cluster simplificado a
  esfera do grupo inteiro (~16 clusters), grande demais para cair atrás de alguma coisa e frouxa no frustum. A esfera do
  grupo continua só para o erro do LOD. Só isso tirou 25% dos triângulos do DAG (104 → 78 mil). Cache `_dag2.bin`.
- **Índices alocados exatamente** (passe 8 do `cull.comp`, entre o 6 e o 7): soma o que o passe 6 viu de cada instância
  e reserva só isso no `cl_idx`.
  - Antes, o passe 0 reservava o LOD 0 inteiro por instância (4,6 M índices por rocha): 64 MB só davam para 3 rochas. As
    demais saíam inteiras pelo caminho antigo, inclusive a rocha da frente na `rocks`.
  - Os cards Masked também passaram por isso: 9 instâncias × 1,8 M índices já passavam perto do limite.
- **Dispatch próprio dos DAGs:** a fase 1 tem uma lista e argumentos próprios (`CL_DAGLIST_AT`, palavras 5–7 do
  cabeçalho). O y dos Masked deixou de pagar os 393 blocos de uma rocha, e o dos DAGs entra na 2ª fase só quando ela
  roda.
- **Estatísticas:** os triângulos desenhados por clusters agora entram em `stats.triangles`. `REALENGINE_DAG_STATS=1`
  imprime instâncias e triângulos por fase.
- **Medido** (Épico, 1440p, mínimo da GPU, GPU livre):
  - `rocks`: sem DAG 5,60 → **4,62 ms**.
  - `rocks`, vista ao longo da fileira: 6,67 → **5,27–5,37 ms**.
  - `heavy`, A/B intercalado de 4 rodadas: 12,73 × 12,76 ms. Ali nenhuma instância usa o DAG.

### Nanite Tessellation sobre a DAG (Q17, 2026-11-18)
O Displacement de um material com Enable Tessellation move de verdade os clusters do corte: cada triângulo base vai para o
`tess.comp` (fatores por aresta pelo tamanho na tela), desenhado por padrões em lote pelo `tess.vert` (Q22; antes o passe 9 do `mesh.vert`), sem costura.
As cópias de vértice a um fio de distância passaram a ser soldadas na posição antes da DAG (a costura da rocha da `rocks`
virava T-junctions). Detalhes e medidas: [tessellation.md](tessellation.md).

## Otimização geral, fase 4: CPU e GPU-driven, medida (2026-11-07)
A fase previa levar à GPU o que ainda roda na CPU por quadro. Antes de mexer, medi quadro, CPU e GPU de cada cena
(Épico, 1440p, GPU livre).

| Cena | Quadro | CPU do quadro | GPU (média / mínimo) | CPU do render (`render_scene`) |
|---|---|---|---|---|
| heavy | 14,8 ms (68 fps) | 3,0 ms | 14,8 / 12,7 ms | 0,8 ms* |
| rocks | 5,4 ms | 3,0 ms | 5,4 / 4,6 ms | — |
| casa assombrada | 6,9 ms | 3,1 ms | 6,9 / 6,1 ms | — |
| RPG | 5,9 ms | 3,5 ms | 5,9 / 5,4 ms | 0,87 ms |
| house | 2,8 ms | 2,5 ms | 2,8 / 2,4 ms | 0,45 ms |
| multidão de 20 000 | 7,6 ms | 4,5 ms | 7,5 / 5,9 ms | 0,66 ms |
| 1 M objetos, `--gi full` | 5,9 ms | 3,2 ms | 5,9 / 5,4 ms | — |
| clínica (terror) | 5,5 ms | 3,3 ms | 5,5 / 5,0 ms | — |

\* depois da correção abaixo.
- **Todas as cenas estão limitadas pela GPU:** o quadro é o tempo da GPU. O render já é dirigido pela GPU: culling, LOD,
  DAG, clusters, multidão por compute, cena persistente, oclusão. Ele gasta 0,45–0,87 ms de CPU. O resto da CPU do
  quadro (UI do editor, sistemas) fica abaixo da GPU, e no jogo empacotado não há UI do editor. Levar mais coisa para a
  GPU não muda o fps hoje.
- **Regressão corrigida (desta rodada):** a leitura das estatísticas dos clusters lia os 3 × 4096 draws de memória do
  host sem cache, e isso custava ~1,1 ms de CPU por quadro (CPU do render da `house` 1,14 → 2,29 ms). Agora o
  cabeçalho vai junto na cópia, e só as entradas usadas são lidas (0,95 ms com validação).
- **Onde o tempo está** (Debug Viewer na `heavy`): ~8,7 ms são sombras por raio do sol e da luz de área atravessando as
  camadas de cards com teste de alfa. É o que a fase 5 ataca (opacity micromaps).

## Otimização geral, fase 5: opacity micromaps e virtual texturing, medidos (2026-11-07)
O maior custo que sobra na `heavy` são as sombras por raio atravessando as camadas de cards com teste de alfa:
12,70 ms contra 8,95 ms tratando os cards como opacos nos raios (`REALENGINE_NO_RT_ALPHA=1`, medida), ou seja, 3,75 ms.
- **Opacity micromaps** (`VK_EXT_opacity_micromap`, opt-in: `REALENGINE_OMM=1`; `REALENGINE_OMM_LEVEL=1..6`, padrão 3):
  - **`omm.comp`:** divide cada triângulo de uma malha Masked desenhada por um só material em 4^n micro-triângulos, na
    ordem "bird curve" da especificação.
    - Amostra o alfa assado no mip que o raio lê: 15 pontos por micro-triângulo, ou 6 a partir do nível 4.
    - Marca cada um como opaco, transparente ou desconhecido (4 estados, 2 bits).
  - **`gi_blas_omm`:** monta o micromap e refaz o BLAS da malha com ele, sem a flag opaca.
    - `MESH_INFO_OMM` tira o `FORCE_NO_OPAQUE` das instâncias no `tlas_fill.comp`.
    - Os dois TLAS são refeitos no mesmo quadro. Um TLAS só reajustado continuava traçando o BLAS liberado, e a GPU caía
      (`VK_ERROR_DEVICE_LOST`).
    - O tamanho do BLAS precisa das contagens de uso do micromap (`usageCountsCount`).
  - **Medido nos cards da `heavy`** (600 k triângulos, folhas de voronoi que ocupam a textura inteira):

| Nível | Micro-triângulos (opacos / transparentes / desconhecidos) | Dados | GPU (mínimo) |
|---|---|---|---|
| sem | — | — | 12,7 ms |
| 3 | 0% / 24% / 76% | 9,6 MB, 0,25 s | 12,76 ms |
| 4 | 3% / 44% / 53% | 38 MB, 0,9 s | 12,73 ms |
| 5 | 12% / 58% / 29% | 154 MB, 3,8 s | 12,86 ms (passe main 11,49 → 10,72) |

  - **Sem ganho na RTX 3060**, nem com 71% dos micro-triângulos decididos e A/B intercalado. O driver aceita a extensão,
    mas o hardware de OMM é das séries 40 e 50. Fica **desligado por padrão** (nada entra sem medida), pronto para ser
    medido numa GPU com esse hardware.
- **Virtual texturing:** não feito. A textura não pesa no quadro: na `heavy`, com o orçamento de 96 MB (a rocha em 512 em
  vez de 4096), o mínimo fica igual (12,79 × 12,68–12,75 ms). O ganho do VT seria escala de memória (mundos de texturas
  únicas), e o streaming com orçamento (`materiais.md`) já cobre isso hoje.
- **O que daria mais fps na `heavy`:** menos raios com alfa. Exemplos:
  - um proxy grosso dos cards só para as sombras (o LOD desbastado, com a mesma cobertura);
  - a sombra do sol só pela VSM nos cards (o híbrido já não traça raio neles).

## Estilos de render (2026-11-09, plano de melhorias A9.0, A9.4 em parte, A5a)
O estilo diz **como o quadro fica**; a escalabilidade, **quanto custa**. `render_style()` (render.h): o do projeto
(Configurações do Projeto > Rendering > Render Style, `project.render_style`) ou o que um **Set Render Style** pôs
enquanto o jogo roda (−1 volta; o fim do Play volta; `--render-style N` nos testes). Vai no `pp[7].w` do quadro: o
FrameBuf não mudou. Tudo na composição (`composite.frag`), depois da exposição e do color grading:
| Estilo | O que faz |
|---|---|
| Estilizado (0) | ACES, a luminância em 4 faixas chapadas (a cor mantida), saturação 1,3 |
| **Realista (1)** | ACES como sempre (o mesmo código: imagem idêntica). **O padrão** |
| Fotorrealista (2) | **AgX** (Sobotka, o do Blender; a sigmoide pelo ajuste polinomial de Wrensch, sRGB linear) |
| Vídeo-realista (3) | AgX + distorção de barril (k 0,035), franjas ≥ 0,15, **halation** (o bloom tingido de vermelho), grão do sensor ≥ 0,04 (mais forte no escuro), vinheta ≥ 0,3 |
- O R11 manda o vídeo-realismo virar o padrão **depois** do teste cego e da aprovação do Nathan: até lá o Realista
  segue padrão e os outros são opção.
- **Mesma exposição:** o AgX recebe o mesmo 0,6× do ACES: o cinza médio sai perto do mesmo brilho.
- **Falta** (A9.4 e A5a): ISO / f-stop / obturador como modo de exposição, bokeh físico, motion blur pelo obturador,
  flare, presets de câmera (Celular, Ação, Cinema, 35 mm, Segurança, Bodycam), saída HDR10, LUT 3D no Post Process Volume.
- Nós **Set Render Style** e **Get Render Style**; testes `render_style_stylized`, `render_style_photoreal`,
  `render_style_video` (cena materials, validação, > 200 fps); artigo `render-styles`.

## GTAO, a oclusão de ambiente em tela (2026-11-09, plano de melhorias A4a)
O AO analítico (32 oclusores, `contact_gi`) só vê os atores Movable perto; os cantos do cenário (parede com chão, com
teto) ficavam sem a sombra suave de oclusão. Agora, do Alto para cima (`gi.gtao`), um compute depois do passe principal:
- **Sem pré-passe nem alvo novo:** o `mesh.frag` (todas as variantes) põe no alfa do HDR a parte do pixel que é luz
  difusa indireta, como 1 + parte (o céu, os modos sem luz e os translúcidos deixam 1); o `gtao.comp` escurece só essa
  parte e devolve o alfa a 1. O sol e as lâmpadas ficam com as sombras deles; o AO analítico continua multiplicado.
- **O cálculo:** horizontes em 2 fatias × 6 passos para cada lado, raio de 60 cm no mundo (até 96 px), a normal pela
  profundidade (o lado mais próximo: sem normal atravessando silhueta), as fatias giram por pixel e por quadro (ruído
  de gradiente intercalado) e o TSR as média. **Meia resolução**: uma thread por bloco 2×2, aplicado aos pixels do bloco
  na mesma superfície (profundidade a menos de 3%).
- **Fora:** MSAA (o Cinemático: o alfa é o peso do resolve), o path tracer, os modos sem luz. `REALENGINE_NO_GTAO=1`.
- **Custo** (house, Épico, 1440p, mínimo): passe principal 1,38 → 1,65 ms em meia resolução (era 2,17 em resolução
  cheia). Timestamps 20-21 próprios (`gpu_gtao_ms`, `TS_PER_FRAME` 22); no orçamento ele conta na fatia de pós.
- **Sem custo no resto:** o `mesh_rt` continua com 128 registradores e 16 B (o `shader_spill` passa). Cintilação nos
  testes: gi_house 0,09%, house_walk 0,11%, demo_rpg_flicker e demo_horror_flicker passam.
- **Falta** (A4d-e): oclusores do AO por tile, refração em tela; a bent normal. (A4b sombras de contato e A4c SSR: abaixo.)

## Reflexos em tela, SSR (2026-11-09, plano de melhorias A4c)
Onde nenhum raio traça reflexos (`GI_REFLECTIONS` desligado: Baixo, Médio, GPUs sem ray tracing).
- **Sem alvo novo nem valor vivo até o fim do shader:** no `mesh.frag`, no ponto em que o reflexo por raio substitui o
  `env` (o céu e as sondas), `ssr_trace` marcha pela cor e pela profundidade do **quadro anterior** (cópias feitas ao fim
  de cada quadro depois do GTAO: `ssr_col` RGBA16F, `ssr_dep` D32, bindings 82-83, no tamanho de render) e o `env` vira
  o da tela pela confiança. 24 passos crescendo 1,3x a partir de 8 cm (até ~34 m), cada ponto projetado uma vez como
  `A + B t` pela `cloud_prev_vp` (a view do quadro anterior), comparado com `near / profundidade` lá; acerto: na frente
  por até `max(20% de t, 12 cm)`. Confiança: bordas da tela e passos longe; rugosidade 0,25 a 0,45 vai sumindo.
- **Registradores:** na variante simples (`mesh`) o laço derramava 64 B: o SSR fica só nas variantes com
  `RE_LOCAL_SM` (como as sombras das luzes locais) e o `render.c` usa a `mesh_lsm` quando o `GI_SSR` está ligado. Ficou:
  mesh 0, mesh_lsm 0 (antes 48), mesh_rt 16 (igual). `[[dont_unroll]]` no laço.
- **Cópias:** `hdr`/`tsr_in` e `depth_ms` ganharam `TRANSFER_SRC`; as cópias começam em SHADER_READ_ONLY (o set as nomeia
  assim desde o início); a primeira vale no quadro seguinte (`ssr_valid`).
- **Teste** `ssr` (`--ssr-test`: o chão do nível padrão vira espelho, `M_Grid` metálico com rugosidade 0,04; Médio,
  validação). Visto: o cone, o cubo e o cilindro refletidos no chão; desligado, só o céu.
- **Custo** (Médio, 1440p, o espelho na maior parte da tela, o PC ocupado por outro programa): GPU 3,45/3,60/3,65 ms
  com x 3,20/3,32/2,92 sem, ~0,3-0,4 ms. `REALENGINE_NO_SSR=1` desliga.
- **Ajuste (suíte final):** 24 passos x 1,3 custavam 0,7 ms na multidão de 20 mil (98 x 105 fps, agentes lisos): agora
  16 passos x 1,45 (o mesmo alcance) e rugosidade até 0,35 (some de 0,2 a 0,35): 107 x 107 fps; o espelho segue refletindo.
- **Falta:** meia resolução com reconstrução, reflexo áspero (cone), objetos fora da tela (só o raio), Hi-Z.

## LUT de correção de cor no Post Process Volume (2026-11-09, sobras da Fase 2)
Como o Color Grading LUT da Unreal. `CPostProcess.lut` ("/Content/...") e `lut_intensity`, no fim do componente (os níveis
antigos continuam abrindo: o ecs.c lê as linhas no tamanho antigo e o resto fica no padrão).
- `post_blend_at`: o LUT do volume de maior prioridade com peso > 0 na câmera, intensidade x peso; vai para o
  `RenderView.grade_lut`.
- `render.c`: binding 80 (a faixa N·N x N RGBA8), identidade 16³ criada no primeiro `update_descriptors`;
  `lut_follow` lê o arquivo quando o caminho muda (.cube 2..64 com DOMAIN_MIN/MAX, ou PNG N·N x N), espera a GPU uma vez e
  aponta o set para a faixa nova. `GpuFrame.grade_lut` (+16 B): intensidade e N.
- `composite.frag`: depois do tone mapping (e do Estilizado), antes da vinheta e do grão; a cor em sRGB, as duas
  fatias de azul em volta, bilinear em vermelho e verde, de volta ao linear; `mix` pela intensidade.
- Details: "Color Grading LUT" (texto + "..." que só aceita arquivo dentro do Content) e a intensidade.
- Teste `color_grading_lut` (`--lut-test`: um .cube 17³ que inverte as cores num volume ilimitado; validação). Visto:
  as cores invertidas (cone azul laranja, cilindro vermelho ciano).
- Custo: duas leituras de textura por pixel no composite (só com intensidade > 0).

## Sombras de contato do sol em tela (2026-11-09, plano de melhorias A4b)
O mapa do sol não tem resolução para o último centímetro (pé de cadeira, pedra no chão: o objeto parece flutuar).
- **Onde:** só quando o sol é sombreado só pelo mapa: sem `GI_RT_SHADOWS` (com ele há o híbrido ou todas as luzes por
  raio) e sem `GI_REFLECTIONS` (as variantes RT não carregam a parte do sol). Na prática: Baixo e GPUs sem ray tracing.
  `gi.contact_shadows` (ligado em todo preset; a condição acima decide), `REALENGINE_NO_CONTACT=1` desliga.
- **mesh.frag** (só as variantes sem RE_RT: nas RT a float a mais derramava 32 B dos 128 registradores): a luminância
  da luz do sol (`sun_l`) vira a parte do sol no pixel (pelo ar e pela neblina como o GTAO), em 15 níveis, no alfa:
  `1 + gshare + 2 x nível`. Em meia precisão o alfa vai até 32 com passo de 1/64: o gshare continua preciso.
- **gtao.comp** (o mesmo passe; despachado com o GTAO ou o contato): decodifica as duas partes; para cada pixel com sol,
  12 passos (quadráticos, com jitter por quadro que o TSR média) em direção à luz por `clamp(0,012 x distância, 8, 150)`
  cm, cada ponto projetado pela `view_proj` e comparado com a profundidade naquele pixel; na frente por 0,2 cm até a
  espessura (`max(35% do comprimento, 3 cm)`): sombra (mais suave perto do fim). Escurece só a parte do sol.
- **Teste** `contact_shadows` (Baixo, nível padrão, validação, "Contact shadows: on"). `shader_spill` igual a antes
  (mesh_rt 16 B, como já era). Nível padrão no Baixo: 1061 pixels mudam (os pés dos objetos).
- **Custo:** dentro do ruído no Baixo (GPU 1,50 com x 1,62 ms sem, com o PC ocupado por outro programa); a medida limpa
  fica para a revisão final.
- **Falta:** comprimento por luz (Contact Shadow Length da Unreal), luzes locais, as variantes RT sem o híbrido.

## VRS, sombreamento de taxa variável (2026-11-09, plano de melhorias A9.7b) — opcional
`VK_KHR_fragment_shading_rate` com imagem de taxa (Content Adaptive Shading): antes do passe principal, `vrs.comp` lê o
HDR do quadro anterior (binding 1) e dá a cada bloco de 16 px (o texel da GPU) 2×2 se é liso (contraste < 0,035 em
l/(1+l)), 2×1 ou 1×2 se muda só numa direção (< 0,09), senão 1×1. Só as malhas opacas usam a taxa (combinador REPLACE);
céu, Masked, partículas, água seguem 1×1. Binding 67 (R8_UINT).
- **Ganho** (Épico, 1440p, passe principal mínimo): house 1,67 → 1,23 ms, Oakvale 2,52 → 2,14 ms (~0,4 ms).
- **Por que opcional** (Configurações do Projeto > Rendering > Variable Rate Shading, ou `REALENGINE_VRS=1`): ligado, a
  cintilação dos testes subiu (gi_house 0,09 → 0,13%, house_walk → 0,16%) e o `spec_leak` viu luz passando por paredes
  finas (o pixel grosso sombreia além da aresta do triângulo). E a flag de anexo nos pipelines, mesmo sem a imagem,
  mudava o código do driver o bastante para os testes de cintilação: os pipelines só a recebem com o VRS pedido (trocar a
  opção os refaz). `REALENGINE_NO_VRS=1` tira a extensão.
- **Falta** para ligar por padrão: histerese da taxa entre quadros, a taxa 1×1 em bordas de profundidade (a imagem pela
  profundidade também), reprojetar o quadro anterior pelo movimento.
- Teste `vrs` (house andando, Épico, validação, orçamento de 10 ms).
- **Artefatos corrigidos (2026-11-14, "Sombreamento de taxa variável cria artefatos"):** (1) blocos que cruzam uma borda
  de profundidade do pré-passe deste quadro ficam 1×1 (o pixel grosso sombreava além da aresta: halos, luz em paredes
  finas); (2) a cor do quadro anterior é lida onde o ponto estava (profundidade → mundo → vista anterior): no movimento
  a taxa vinha de um quadro antes; (3) histerese pela taxa do quadro anterior (a imagem de taxa guardada; limiares 30%
  mais justos para engrossar, 30% mais largos para voltar); (4) limiares a um terço (0,0105 e 0,027): textura fina em
  2×2 pegava texels diferentes a cada jitter do TSR. Casa parada, Épico: cintilação sem VRS 0,17%, VRS antigo 0,35%,
  novo 0,19%; passe principal 1,98 → 1,38 ms com VRS. A profundidade passa a `DEPTH_READ_ONLY` durante o `vrs.comp`.

## Denoiser unificado de RT (2026-11-09, plano de melhorias A9.1d) — Fotorrealista
No estilo Fotorrealista (e Vídeo-realista; `REALENGINE_DENOISER=1` liga em qualquer estilo, `=0` desliga) a GI difusa e
os reflexos ásperos viram **raios de verdade** com ruído, filtrados junto com as sombras estocásticas por **um filtro
só** (`denoise.comp`, no estilo do ReBLUR da NVIDIA). O Realista não muda: nem tem o código.
- **Os sinais** (`mesh.frag`, variantes `RE_DEN`): um raio difuso por pixel (cosseno, até 30 m; o acerto iluminado pelo cache de
  superfície, ou pelas luzes e sondas onde não há carta; o erro devolve a GI daqui, `irr`, como campo distante) e um
  raio de reflexo pelo lóbulo GGX (normais visíveis, as calotas esféricas de Dupuy e Benyoub) quando a rugosidade fica
  entre o corte do reflexo nítido e 0,9. Saem da cor e vão para dois alvos novos do passe principal (RGBA32UI): os
  sinais em R11G11B10 pré-expostos, a chave (normal octaédrica 10+10 bits, rugosidade 6, hash do ator 5), o albedo vezes
  a transmitância do ar e da neblina; e o guia do pixel (o peso do reflexo, o ator, a profundidade escrita). Com lightmap
  o difuso fica o do lightmap; com coat ou película (M1b, M4) o reflexo fica o de antes. As amostras: um início por pixel
  (hash) andando pela sequência R2 a cada quadro — o ruído de gradiente intercalado girado por quadro punha as amostras
  de cada pixel num reticulado: um viés fixo que o histórico guardava (grão no piso rasante).
- **O filtro** (3 pipelines especializados, um por passe; 8 + 1 imagens, ~130 B por pixel de render, criadas no primeiro
  quadro que ele roda): (0) pré-borrão de 8 amostras numa espiral de Vogel girada por pixel e quadro (raio 10 px, menor
  num contato pela distância do acerto; reflexo pelo raio da rugosidade; sombra 4 px), os três sinais nas mesmas
  amostras com o peso de cada um pelo próprio raio, uma leitura de 128 bits por amostra; (1) histórico reprojetado pela
  profundidade, as 4 amostras só no **mesmo ator**, normal a menos de 25°, profundidade a 1% (o que falha é descartado,
  nunca misturado), poucos quadros (difuso 64, reflexo 4-24 pela rugosidade, sombra 8), um histórico rápido de 3 quadros
  que corta o longo para 3 quando a luz muda mais de 2× (anti-lag), a visibilidade presa a ±0,25 da atual; (2)
  pós-borrão (4 + 10/√quadros px) e uma estabilização da saída difusa (a de antes presa a ±5% da atual, 15% da nova);
  a cor recebe difuso × albedo + reflexo × peso + luz das sombras × visibilidade. O teste de plano usa a profundidade em
  tela (afim num plano), sem matriz por amostra.
- **Não pesar no Realista:** escrever as duas saídas, mesmo zeros, custava 0,2 ms no passe principal do Realista (−7% fps
  na house). Por isso só pipelines feitos para o denoiser as escrevem (`den_rt`, 16 variantes `-DRE_DEN` de shader),
  numa thread (a frio o driver levou 31-62 s; até ficarem prontos o quadro sai como antes). Uma constante de
  especialização no lugar das variantes, mesmo com o código morto, mudou o código do driver o bastante para reprovar
  `gi_house` (cintilação 0,104 → 0,156%) e `spec_leak` (pico 11 → 65 nits) com a validação: as variantes de sempre agora
  saem com o SPIR-V idêntico ao de antes (as funções de empacotamento em `denoise.glsl`, fora do `common.glsl`). O benchmark espera 0,75 s depois
  de pipelines pendentes (o histórico de 64 quadros convergindo). MSAA fica com os filtros separados.
- **Medidas** (RTX 3060, 1440p, Épico; outro processo usando a GPU deixou as medidas ±5%):
  | | sem | com |
  |---|---|---|
  | house: fps / passe principal | 267 / 1,66 ms | ~144 / +1,5 ms dos raios, filtro ~1,65 ms |
  | house: cintilação | 0,073% | 0,12-0,14% |
  | house com luzes de área (`--source-radius 30`): cintilação | 0,167% (shadow split) | 0,104% |
  | house: erro contra o path tracing (RMSE relativo, linear) | 11,5% | 7,7% |
  | RPG Oakvale (`--cam -300,-600,250,60,-8`): fps / cintilação | 138 / 0,105% | 74 / 0,122% |
  | house_movable: cintilação (ida e volta) | 0,083% (0,18%) | 0,345% (0,27%): a GI responde aos atores |
  A meta do Fotorrealista (60 fps a 1440p com upscaler na house e na RPG) está cumprida. O filtro começou em 3,8 ms:
  as leituras de 128 bits do guia e do sinal por amostra (taxa de 1/4) e a matriz por amostra eram o gargalo.
- **Depurar:** `REALENGINE_DEN_DEBUG` 1 difuso, 2 reflexo, 3 visibilidade, 4 tamanho do histórico (vermelho < 8, azul,
  verde cheio). Timestamps 24-25 (`gpu_denoise_ms`, na fatia de GI do orçamento). Testes `denoiser_house`,
  `denoiser_area`.
- **Falta:** reprojeção pelos vetores de movimento dos atores (hoje a câmera; um ator que anda perde o histórico e fica
  ruidoso alguns quadros), o raio na folhagem Masked mais barato (+5,6 ms na RPG), alimentar o ReSTIR GI (A9.1b) no lugar
  do raio único.

## Pele: subsuperfície em tela (2026-11-09, plano de melhorias A9.3a)
Como o Subsurface Profile da Unreal, para um material com Subsurface Weight e Scale (o Principled BSDF; `mat_sss`):
- **Xadrez no `mesh.frag`** (só nas variantes `RE_EXT`, `GI_SSS`): pixel par (x + y) só a luz difusa (Lambert, sem o
  wrap antigo), ímpar o resto (especular, coat, emissão, a luz da neblina e do ar). Os bits ficam no topo do `g_ext`
  (a tangente da anisotropia caiu para 2 × 9 bits): nenhum registrador a mais (`mesh_rt_x` 128 com 64 B, como antes).
  O alfa leva -(slot + 1) do material (half: exato até 2048; slots acima ficam com o wrap). Cada metade traça só o seu
  raio (reflexo, denoiser): a pele gasta a metade deles.
- **`sss.comp`** depois do GTAO, do denoiser e do split, antes do OIT: passada 0 junta, para cada pixel de pele, a luz
  difusa dos pares em volta pelo perfil de Burley de cada canal (amostras por importância do canal mais largo, raios
  numa tabela compartilhada, peso pela distância 3D: nada vaza por uma silhueta) e o especular (o próprio ou a média
  dos ímpares vizinhos) numa imagem própria; o primeiro pixel de cada tile 8x8 marca o tile. Passada 1: só os tiles
  marcados voltam para a cor (alfa 1) e zeram a imagem atrás (o resto da tela: uma leitura por tile). O GTAO pula os
  pixels de pele.
- **Distâncias** no `e7.zw` do material: o canal mais largo em cm (Radius × Scale) e as partes de cada canal em 8 bits.
  O exemplo `M_Skin` agora tem Scale 0,012 m (o padrão 0,05 do Blender é cera); `M_HumanSkin` igual (um `M_Skin` antigo
  do projeto fica como estava).
- **Transmissão** (orelha, dedo): no laço das luzes, numa luz por trás de um pixel difuso, um raio de dentro mede a
  espessura até 7 × a distância; Beer-Lambert por canal (2 mm deixa passar o vermelho e parte do verde, 2 cm um vermelho
  fraco; o perfil de Burley deixava um terço do vermelho a 2 cm). Usa o orçamento de raios de sombra. Só com RT.
- **Desligada** com MSAA (Cinemático: o alfa é o peso do resolve), com VRS e no path tracer: o wrap antigo.
  `REALENGINE_NO_SSS=1` desliga; `REALENGINE_SSS_RAW=1` mostra o xadrez cru.
- **Medidas** (3060, Épico, cena `skin`, 1617x998 interno): passada 0,196 → 0,127 ms com a tabela e 16 amostras
  (Baixo 6, Médio 8, Alto 12, Épico 16); quadro 2,74 × 2,60 ms sem (353 × 374 fps). Sem material de pele: zero.
- O raio da espessura só aceita a saída pela **própria malha** (`own_hit`: candidatos só do próprio ator): os olhos
  enterrados na cabeça davam anéis vermelhos em volta deles. No caminho por software (A1) ainda o primeiro acerto.
- Com o rosto enchendo a tela (cena `eyes`, 1617x998 interno): passada 0,39-0,55 ms.
- Testes `skin_sss` (Épico) e `skin_sss_low` (Baixo, sem raios). **Falta:** pós-texturização (o albedo é espalhado
  junto: sem albedo guardado), SSS com MSAA, microdetalhe de poros, cabelo em fios (A9.3b).

## Olhos: o Shading Model Eye (2026-11-09, A9.3c)
Material > **Shading Model: Eye** (`MATF_EYE`, `Material.shading_model`, salvo como `"shading_model":"Eye"`):
- A **córnea** é um coat (o caminho do M1b, normal geométrica, rugosidade 0,05, F0 do IOR 1,376) sobre o olho todo; a
  base sem especular própria (F0 0). Nada novo no `brdf`: os reflexos e o coat já existiam.
- A **íris** (centro do UV, `e7.x` o raio, `e7.y` a altura da córnea em cm) lida **refratada**: antes das texturas, o
  raio da vista refratado pela córnea anda até o plano da íris e a uv anda junto (pelo referencial das derivadas: du =
  dot(o, T) / det). A altura vai a 0 no limbo: sem emenda nas derivadas.
- A **normal da íris** é um prato virado para o olhar (+X do ator), inclinada para o centro: a luz de um lado se junta
  no lado oposto (a cáustica). A esclera fica com a da esfera.
- Exemplos `Examples/M_Eye` (rampa pela distância ao centro: pupila, duas cores de íris, limbo, esclera, fibras por
  ruído) e a malha `Examples/SM_Eye` (esfera olhando +X, UV planar de frente). Cena `eyes` (os olhos na cabeça da cena
  `skin`, câmera a 20 cm); teste `eyes`.
- **Falta:** a malha de oclusão e a linha de umidade das pálpebras (como as do MetaHuman), a sombra da pálpebra na íris
  só vem das sombras por raio.

## Cabelo em fios: o componente Groom (2026-11-09, A9.3b)
Como o Groom Component da Unreal (`COMP_GROOM`, `CGroom`, `src/groom.c`, `shaders/hair.glsl/.vert/.frag`):
- **Crescer** (CPU, uma vez; de novo quando um número de forma muda: a chave cobre fios, segmentos, semente, tamanhos,
  cachos, gravidade, mechas, couro, testa e a malha): as raízes na elipsoide das bordas da malha do ator (acima de
  `scalp` em z, sem as da frente abaixo do topo: `hairline`, o ator olha +X), penteadas pela tangente para baixo e para
  trás, cada segmento puxado pela gravidade e mantido fora da cabeça (desliza nela); cachos em hélice em volta do
  caminho; mechas: guias primeiro (fios / 80), cada fio puxado para o caminho da guia mais perto (mais na ponta). 20 mil
  fios × 15 pontos em ~30-40 ms. A grade de densidade 32³ (comprimento × largura por cm³: a extinção, 1/cm) junto.
- **GPU:** um buffer local com os pontos (vec4 no espaço do ator) e as grades, por um staging copiado antes do passe
  principal (`groom_record`); uma mudança espera a GPU (rara). Por quadro só a tabela (matriz, inversa, LOD, cor).
- **Desenho:** cada segmento uma fita virada para a câmera (`hair.vert`, 6 vértices), largura do fio e no mínimo um
  pixel; a parte que falta vira cobertura. Pré-passada de profundidade (`hair_depth`: corta a cobertura por um hash de
  pixel, fio e quadro: o TSR tira a média) e depois o sombreamento com profundidade EQUAL e `early_fragment_tests`:
  cada pixel uma vez. Com `discard` no sombreamento eram 123 fps na cena `hair`; com a pré-passada 205.
- **Luz** (`hair.frag`): Marschner como Karis (2016): R, TT, TRT (gaussianas longitudinais com o desvio da cutícula,
  termos azimutais), mais o espalhamento múltiplo por um wrap tingido; cor da fibra pela melanina (absorção de eumelanina
  e feomelanina, Chiang 2016) × Dye. Cada luz (as direcionais, o sol pelo VSM, e as locais da célula) pela
  transmitância na grade de densidade (sombra profunda, 16 passos); ambiente pelas sondas (ou o céu) com a oclusão da
  grade para cima.
- **Alvos do passe:** variantes `hair` (1), `hair_o2`, `hair_o4` gravam zero no split e no denoiser: nada da malha de
  trás é filtrado para cima do cabelo (o tecido e a água ainda deixam esses alvos como estavam: sobra).
- **LOD:** abaixo de 350 px de raio na tela, menos fios (até 6%) e os outros mais largos (a mesma cobertura).
- **Medidas** (3060, Épico, cena `hair`, a cabeça a ~400 px, 20 mil fios / ~11 mil desenhados): +0,3-0,7 ms na GPU
  (208 × 213 e 173 × 193 fps entre rodadas). `REALENGINE_NO_HAIR=1` compara. Testes `hair` e `hair_low`.
- **Falta:** sombra do cabelo na pele (VSM e raios), velocidade para o TSR (fantasmas ao mexer a cabeça), raízes da
  superfície da malha/pintura de densidade, importar grooms (Alembic, Tlender), física dos fios, cards automáticos ao
  longe, nós de Blueprint.

## Cabelo dinâmico e cabelo do Tlender (2026-11-16)
- **Simulação** (`shaders/hair_sim.comp`, um thread por fio, no mundo): Verlet (o movimento do passo anterior menos o
  Damping, gravidade, vento com rajadas por fase do fio), raiz presa onde o ator põe a raiz crescida, forma puxada para o
  fio crescido (firme na raiz, Stiffness na ponta: a restrição de forma global do TressFX), comprimento pelo FTL de
  Müller 2012 (a correção tirada do histórico também), colisões: elipsoide da cabeça (espaço do ator), até 32 cápsulas
  dos ossos (`anim_skin_palette` × cabeças do bind; raios do Physics Asset quando há) e 16 caixas da cena
  (`bp_world_boxes`), pelo Collision Radius. Os pontos simulados (atual e anterior) ficam no buffer do cabelo depois dos
  estáticos; `hair.vert` lê deles (e a velocidade do TSR pelo anterior). Teleporte ou primeiro quadro: reset.
- **Guias** (`.rgroom`: "RGRM", versão 1, contagem, aparência sugerida, por guia n e xyz em cm no espaço do ator;
  `groom_guides_write`, `guides_get` acha pelo hash do caminho virtual andando o Content, cache de 16). Com `Guides`, o
  `grow` troca a elipsoide por `grow_guides`: raiz num disco em volta da raiz de uma guia (60% do espaçamento médio),
  forma das 3 guias mais perto (1/distância), deslocada para a raiz, Clumping puxa para a guia na ponta.
- **Importar:** `.tlend` — o bloco `hairs` (`hr`, `hsurf`, `hdyn`, `hg`; só fios, não instâncias) vira
  `Imported/Hair/GR_<superfície>_<i>.rgroom` e um Groom no ator da superfície (fios, segmentos de render, largura
  raio × 2000 mm limitada a 0,03-0,5, cor como Dye, dinâmica e Stiffness/Damping do Tlender). OBJ — as linhas `l`
  viram guias e um ator `Hair`. Details > Groom > **Guides** (combo dos `.rgroom` do projeto).
- **Medidas** (3060, Épico): simulação 0,266 ms na GPU com 20 mil fios × 15 pontos (cena `hair_dynamic`, validação
  limpa); crescer de 8 guias 3000 fios × 11 pontos em 2,5 ms (CPU, uma vez). Testes `hair_dynamic`,
  `import_tlend_hair`, `import_obj_hair`.
- **Falta:** sombra do cabelo na pele; colisão fio a fio; cápsulas além de 32 ossos.

## Crash da GPU no passe "skin (ossos e BLAS)" ao adicionar um subnível (2026-11-16, relatórios #135-137)
- Sintoma: VK_ERROR_DEVICE_LOST na 0.39 e na 0.40_b, no MyProject (Delight + Tiger, skins de verdade), logo depois de
  "Subnível Lighting_Night adicionado". Adicionar um subnível chama `bp_components_rebuild`: todas as partes dos
  Blueprints são feitas de novo, então todos os skins têm a primeira dobra no mesmo quadro.
- Causa: a paleta de ossos (`R.skin_pal`) crescia no meio do quadro (`skin_bind`), mas os trabalhos já escritos naquele
  quadro guardavam o endereço da paleta antiga (ossos e pesos dos morphs), liberada logo em seguida: a GPU lia memória
  liberada. Agora os endereços são movidos para a paleta nova.
- Também: os trabalhos guardam o skin pelo lugar na lista e a lista era reordenada no quadro (inserção em `skin_bind`,
  remoção em `skin_sweep`): índices ajustados. E `skin.comp` limita os índices dos ossos ao tamanho da paleta (uma parte
  sem pose ainda tem paleta de um osso).
- Reprodução: `REALENGINE_ADD_SUBLEVEL=<nome>` adiciona/remove o subnível a cada 3 s (5 s após abrir). Numa cópia do
  MyProject: sem a correção, travou na primeira adição (em segundo plano, com e sem ray tracing); com ela, 19 adições e
  remoções em 70 s, validação limpa. Os Mannequins das cenas de teste são partes rígidas por osso (não usam a paleta),
  então a cena `skin_churn` (48 atores, trocas a cada quadro) cobre só a reordenação da lista.

## Gaussian Splatting (2026-11-09, plano de melhorias A9.5d)
Ator **Gaussian Splat** (`COMP_SPLAT`, `CSplat`, `src/splat.c`, `shaders/splat*.glsl/.comp/.vert/.frag`):
- **Leitura:** 3DGS `.ply` (binário little endian, só floats; x y z, f_dc_0..2 → cor 0,5 + C0 · dc, opacity → sigmoide,
  scale_0..2 → exp, rot_0..3 w x y z) e `.splat` (32 bytes: posição, escala, rgba8, rotação em bytes). Eixos do arquivo
  (Z, -Y do 3DGS/COLMAP, Y) e unidade (cm por unidade). Por gaussiana 64 bytes: centro e opacidade, covariância R S Sᵀ Rᵀ
  em cm², cor rgba8 (raiz: o shader eleva ao quadrado). Copiadas para um buffer local por um staging (`gpu_begin_once`).
  140 mil em ~9 ms.
- **Ordenação na GPU (`splat_sort.comp`):** passe 0 por ator: culling (atrás, fora da vista com a margem de 3 sigmas,
  opacidade < 1/255), chave de 16 bits da profundidade em log2 (10 cm a 100 km, longe primeiro), histograma por atômicos;
  passe 1 (um workgroup): prefixo exclusivo dos 65536 bins (Hillis-Steele em 1024 linhas) e os argumentos do draw
  indireto; passe 2: cada uma no seu lugar (prefixo + atômico).
- **Desenho:** passe próprio sobre a cor (uma amostra) com a profundidade da cena só leitura, depois do fogo e antes dos
  translúcidos; `splat.vert` projeta a covariância (EWA: Jacobiano da perspectiva × eixos da vista, + 0,3 px² como o
  3DGS), quadrado de 3 sigmas; `splat.frag` o peso pela cônica × opacidade, pré-multiplicado, de trás para frente. Cor
  como albedo sob o céu e o sol (Lit by the Scene) ou luz própria em nits.
- **Medidas** (3060, Épico, cena `splat`): 140 mil gaussianas (arbusto de 100 mil, canteiro de 40 mil) 238 × 357 fps,
  +1,4 ms; com folhas grandes (quadrados de ~35 px, 135 M fragmentos) eram +3,35 ms: o custo é a sobreposição na tela.
  `REALENGINE_NO_SPLATS=1` compara. Teste `splat`.
- **Falta:** rasterização em tiles (cenas de milhões), bandas SH 1-3 (cor pelo ângulo), sombras e oclusão no GI,
  importar pelo Content Browser, LOD por distância.

## Async compute (2026-11-09, plano de melhorias A9.7c) — opcional
- **vkctx:** uma fila de uma família só de compute (a família 2 da NVIDIA; `REALENGINE_NO_ASYNC_QUEUE=1` não cria),
  pool e command buffer por slot, timeline própria (`async_timeline`). `vkctx_async_begin/submit`: o lote espera o quadro
  anterior na timeline gráfica (ele leu o que o lote escreve) e sinaliza o valor seguinte; `vkctx_split` envia a
  primeira parte do quadro sem esperas e segue gravando no segundo command buffer do slot (`cmd2`); o `vkctx_end_frame`
  espera o valor assíncrono nos estágios dos consumidores. `vkctx_cmd()` dá o command buffer atual (main.c, game_main.c
  e o caminho de VR pegam ele depois do `render_scene`). Os marcadores da GPU seguem o command buffer atual.
- **Buffers** em `CONCURRENT` entre as duas famílias sempre que a fila existe (em buffers não custa: sem compressão);
  as imagens seguem exclusivas (nada do lote assíncrono usa imagem).
- **O trabalho:** a simulação das partículas (`particles_record(..., async)`: as barreiras só de compute/transfer; o
  FrameBuf lido da cópia no host, porque a do dispositivo é escrita mais adiante no command buffer gráfico). O quadro
  é dividido antes do passe principal (os únicos consumidores: os draws de malhas e sprites).
- **Ligar:** Project Settings > Rendering > Async Compute ou `REALENGINE_ASYNC_COMPUTE=1`. Desligado de fábrica.
- **Medidas** (RTX 3060, 1440p, o PC ocupado por outro programa): particles5m 189/224 fps com x 191/228 sem (os mesmos
  3,49 M sprites); cena `particles` no Épico GPU 4,10/4,21/4,18 com x 4,00/4,03/4,16 sem: igual ou 0,1 ms pior. A
  simulação é curta demais para pagar a submissão a mais e o começo do quadro tem pouco para sobrepor. Fica opcional.
- **Teste** `async_compute` (cena particles com validação e async ligado: as partículas desenhadas, sem erros).
- **Falta** (onde haveria ganho): levar as sondas e o cache de superfície da GI (com transferência de dono das imagens
  ou imagens concorrentes) para a fila assíncrona, sobrepostos às sombras.

## Barras/grade que andavam na tela (2026-11-10)
- O grão de filme (Vídeo-realista força 0,04) e o jitter do motion blur usavam `fract(sin(dot(xy + frame*61,7, k)) *
  43758)`: com coordenadas de 1440p mais até ~63 mil do contador o `sin` perde precisão e desenha linhas e grades que
  andam. Trocado por um hash inteiro (PCG, `noise01` no `composite.frag`): ruído branco sem estrutura, mesmo custo.

## Estilizado sob controle do usuário (2026-11-10)
- Project Settings > Rendering > Render Style = Estilizado abre a seção **Stylized**: preset (Cel, Quadrinhos,
  Pintado, Stop-Motion, Toon), faixas (2-12), suavidade, tom das sombras, saturação, contorno (px a 1440p, cor,
  sensibilidade pela profundidade) e taxa de quadros da animação (`project.h stylized_*`).
- GPU: `GpuFrame.style[3]` (+48 bytes); o contorno lê a profundidade no `composite.frag` (força a variante RE_POST
  só com contorno > 0): 4 leituras por pixel. A animação "em dois" está no `anim.c` (pose a cada 1/N s).
- Padrão igual ao visual anterior (4 faixas, saturação 1,3, sem contorno).

## Grão e Denoise nas Configurações do Projeto (2026-11-10, "agora as listras viraram ruído")
- O "ruído" que sobrou era o grão do Vídeo-realista (mínimo 0,04, novo a cada quadro). `film_grain` (0..2, padrão 1)
  multiplica o grão do estilo e o dos Post Process Volumes (`style[2].w`).
- `image_denoise`: Automático (Fotorrealista e Vídeo-realista, como antes), Ligado (o denoiser unificado de RT em
  qualquer estilo: histórico só do mesmo ator, normal < 25°, profundidade a 1%, anti-lag) ou Desligado.
  `REALENGINE_DENOISER` ainda manda.

## Opções por estilo de render e ruído das sombras (2026-11-10)
- Configurações do Projeto > Renderização > **Opções do Estilo**: **Remover Ruído das Sombras** (Baixo 5x5 / Médio 7x7,
  o de antes / Alto 11x11 / Épico 15x15 no `shadow_split.comp`; o histórico guarda 80 / 90 / 93 / 95% e a caixa de
  vizinhança cresce no Alto e no Épico; o padrão novo é Alto). No Video-realista: **Distorção, Franjas, Halação e
  Vinheta** como multiplicadores do estilo (`GpuFrame.style[3..4]`, +32 bytes).
- Malhas: as classes Masked / Translucent / RE_EXT por malha eram marcadas e nunca limpas: um material que deixou de ser
  translúcido (Transmissão de volta a 0) ficava quebrado. Agora `mat_update` limpa as marcas e as instâncias as refazem.
- Prévias (`render_preview.c`): pele (skins v2) deformada na CPU pela paleta da pose num buffer por quadro; antes só as
  partes antigas apareciam (a Delight ficava vazia). Custo: ~3 ms de CPU com a Delight aberta (só no editor).

## Lóbulos extras em Masked e Two Sided (2026-11-10)
- As corridas masked (Masked, Two Sided: cabelo, folhas, cartões) ganharam variantes RE_EXT: raster (`mesh_m_x`,
  `mesh_m_eq_x`) e as 4 de raio (`RT_PIPES` 8 -> 12: masked, masked_eq e as do terminator), cada uma com o alpha dos
  raios de sombra e a do denoiser (18 shaders novos). Feitas só quando um material Masked/Two Sided tem lóbulos
  (`R.any_ext_masked`), escolhidas no passe principal no lugar das simples; `ext_pipelines_ensure` faz só as RE_EXT
  (`build_rt_pipelines_ex(only_ext)`: antes fazia o conjunto inteiro e jogava fora, 38 s com o cache frio; agora 17 ms
  com o cache quente). `REALENGINE_NO_MASKED_EXT` desliga (a medida do "antes"). O Translucent continua sem RE_EXT.
- Medida (`--scene masked_lobes`: 80 cartões com coat e anisotropia sobre a tela toda, Épico com RT, 1440p, RTX 3060):
  sem 4,37 ms de GPU (principal 2,69), com 4,58-4,64 (principal 2,82-2,84): os lóbulos custam ~0,14 ms no passe
  principal. A cena `heavy` (Masked sem lóbulos) não mudou: 2,84-2,85 ms antes e depois. Testes `masked_lobes`
  (Épico, RT) e `masked_lobes_low` (Baixo, sem RT), validados.

## Raios da GI do denoiser em compute (2026-11-14, notas-dev/desempenho.md)
- `den_trace.comp` traça o raio difuso e o especular áspero depois do passe principal (o `mesh.frag` RE_DEN só
  empacota os parâmetros no `o_den`): o fragment shader com RT deixou de carregá-los. MyProject, Vídeo-realista,
  Épico 1440p nativo: 45 → 65 fps (com o checkerboard e os LODs de pele). `REALENGINE_NO_RT_REFLECTIONS=1` mede o
  reflexo por raio (o SSR no lugar).

- **Quadrados escuros/pretos com VRS (2026-11-14, "sombreamento com taxa variável cria artefatos de quadrados"):** um
  pixel grosso (2×2, 2×1) escreve no `o_guide` a profundidade do centro dele nos 2 ou 4 pixels que cobre; o `denoise.comp`
  comparava com a profundidade do pixel (1e-5) e tomava como "outra superfície por cima": a GI desses pixels caía (blocos
  escuros no chão, pretos nos cubos na sombra). E o `den_trace.comp` lançava os raios desse ponto deslocado (debaixo de
  um chão rasante). Agora o teste aceita o passo do plano (o vizinho mais próximo em cada eixo, lido só quando o teste
  exato falha), a história guarda a profundidade do próprio pixel, e os raios partem da profundidade do buffer.

## Histórico temporal dos atores em movimento (2026-11-15, "depois o histórico temporal dos atores em movimento")
- **Passe de velocidade antes do denoiser**: o "2b" (velocidade dos atores que se moveram) agora roda logo depois do passe
  principal, antes do GTAO e do denoiser (antes: depois deles, só o TSR/DLSS/motion blur o liam). A transição final
  libera para fragment e compute.
- **Denoiser pelo movimento dos atores**: `denoise.comp` reprojeta o histórico pela câmera **menos o movimento do ator**
  (`u_velocity`, binding 40, quando `tsr.z`); num pixel que se moveu o teste de profundidade fica em 8% (a profundidade
  do quadro anterior não é a deste ponto pela câmera), o mesmo ator e a normal parecida bastam. Antes um personagem
  andando perdia o histórico onde estava e ficava ruidoso alguns quadros.
- **Cabelo em fios com velocidade**: o `GpuGroom` guarda as rows do quadro anterior (`prev`, 176 → 224 B); variante
  `HAIR_VELOCITY` do `hair.vert` desenha os fios no passe de velocidade (o ponto em espaço do ator pelas rows de antes).
  Só quando algum groom se moveu (`groom_moving`). Antes o cabelo só tinha o movimento da câmera: rastro no TSR e no DLSS.
- **Tecido com velocidade**: variante `CLOTH_VELOCITY` do `cloth.vert` lê `H.prev` (o início do passo: o quadro anterior
  com 1 subpasso) e desenha o tecido no passe de velocidade. Antes a saia simulada deixava rastro.
- Falta: a cobertura do cabelo (`hair.frag` descarta parte dos pixels; a velocidade cobre o fio inteiro), partículas e
  translúcidos (máscara de reativos).

## A chuva fora das variantes comuns do passe principal (2026-11-16)
- A chuva nas superfícies (`rain_shade`, gotas nos atores, a película) fazia o driver jogar 48 bytes para a memória local
  no `mesh_rt` e no `mesh_rt_t` desde a 0.34 (era 16 + 16). Isso existia mesmo sem chover: o passe principal ficava 2,8%
  mais lento (house Épico, mínimo do main: 1,80 ms contra 1,75 sem o código da chuva; A/B alternado, 3 rodadas).
- **Agora** o código da chuva vai só nas variantes RE_EXT (`_x`) e na RE_LSM. O render usa as `_x` nos quadros em que
  chove (`fr->water_addr[0]`). Na primeira chuva elas são criadas como as dos lóbulos extras (`any_ext` /
  `any_ext_masked`); materiais e decals marcam `ext_mat` / `ext_mat_masked`. Sem chuva, os shaders comuns ficam como
  antes da chuva: `mesh_rt` 128 registradores com 16 bytes, `mesh` 96 sem spill. Main mínimo: 1,73–1,75 ms, igual ao
  build sem chuva. Testes: `shader_spill` passa e a área `agua` também.

## Refração no vidro (2026-11-16)
- **Material**: Refraction (0 desligado, 1 real; padrão 1), Refraction Thickness (cm, padrão 5) e Dispersion (0..1), em
  Translucent ou com Transmission. `MATF_REFRACT` com o IOR do Principled em `e5.w` e espessura, dispersão e força em
  `e7.xyz` (vidro não tem parallax, olho nem pele). O `ext` sobe à GPU também sem `MATF_EXT`.
- **Passe OIT**: terceiro alvo (`R.oit_refr`, RGBA16F somado como a cor, binding 84). `mesh.frag` RE_TRANSLUCENT soma o
  deslocamento na tela de `i_world + (refract(-v, n, 1/IOR) + v) × espessura × força`, a rugosidade e a dispersão, × o
  peso do WBOIT.
- **Composição** (`shadow_split.comp` passe 3, com `GI_REFRACT`): onde há vidro, o que está atrás é lido deslocado (média
  dos deslocamentos ponderada) da cópia do cenário opaco (`ssr_col`, binding 82). A cópia do SSR já é feita antes do OIT
  e agora também nos quadros com vidro refrativo, mesmo sem SSR. Áspero: 5 amostras num disco que cresce com a
  rugosidade (fosco). Dispersão: vermelho e azul desviados para lados opostos.
- Vidraça plana quase não desloca (fisicamente certo). Bola de vidro inverte e amplia. Cena `glass` ganhou uma bola de
  vidro (30 cm, dispersão 0,35).
- **Medida** (RTX 3060, 1440p Épico, cena `glass`, 404 vidros + a bola, mínimo da GPU, 2 rodadas):
  4,864/4,890 ms sem (`REALENGINE_NO_REFRACT=1`) contra 4,962/4,984 com: **+0,1 ms**.

## Cabelo para muitos personagens: orçamento e simulação dos mais perto (Q27.3, 2026-11-23)
Do GTA 6 (auditoria-gta6.md): cabelo em fios com física em todos os personagens, NPCs inclusive, um dos motivos citados
para o jogo não ter 60 fps. Aqui:
- **Orçamento de segmentos por quadro** (`HAIR_BUDGET_SEGMENTS` 1,5 M): cada Groom pede seus fios pelo tamanho na tela
  (o LOD de antes); passou do orçamento, todos são reduzidos pelo mesmo fator (nunca abaixo de 4% dos fios), os fios
  ficando mais largos (`cover`: a mesma cobertura).
- **Simulação só nos 16 mais perto** (`HAIR_SIM_MAX`): os outros dinâmicos são desenhados como cresceram e ganham
  `sim_reset` (ao voltar a simular, recomeçam do repouso, sem salto). `GROOM_MAX` 64 → 256.
- **Medidas** (`--scene hair_crowd`: 100 cabeças com 6000 fios × 12 segmentos, dinâmicos, câmera na primeira fila;
  RTX 3060, 1440p): 36 856 de 600 000 fios desenhados (442 mil segmentos, o LOD pela tela já bastou: fator 1,0), 16
  simulados: **257 fps, GPU 3,9 ms**; todos simulando (`REALENGINE_HAIR_NO_BUDGET=1`, como antes): 121-129 fps, GPU
  7,7-8,2 ms.
- **Teste `hair_crowd`**: 100 Grooms, segmentos dentro do orçamento, 1 a 16 simulados, ≥ 200 fps.
- Próximo: cards gerados e malha com textura no couro para os bem distantes; vento barato no vertex shader dos não
  simulados.

## Nanite, Fallback e colisão por malha; DAG em peles (item 4 parte 2, 2026-11-25)
- **`.rlod` v2** ao lado da malha (`.<arquivo>.rlod`, JSON): `nanite` (-1 Auto: DAG a partir de 16 384 triângulos; 1 On:
  de 512; 0 Off), `fallback` (% dos triângulos do LOD 0 que a BLAS usa: o LOD mais simples que ainda os tem) e `collision`
  (0 do ator, 1 simples, 2 complexa: `physics.c` `geom_of` lê `render_mesh_collision`). O v1 (só LODs) abre como antes.
- Visualizador de Static/Skeletal Mesh, seção **Nanite Settings**: Enable Nanite Support, Fallback Triangle Percent,
  Collision Complexity, Apply Nanite and Collision (salva e recarrega as malhas), Generate Lightmap UVs (não skinned) e as
  linhas só de leitura: clusters do DAG, LOD/triângulos/erro do fallback, pele rígida (`render_mesh_nanite`).
- **Peles no DAG**: pele parada, rígida e viva (as vivas com as esferas folgadas, notas-dev/animacao.md "Nanite na pele
  viva"); `cull.comp` só exclui a Masked viva e o soft body. `REALENGINE_NANITE_ALL=1`: toda malha sem `.rlod` em On.
- Regressão: `cluster_lod` 146 fps, `heavy_mesh` 122 fps, `crowd_20000` 115, `crowd_panic` 221 (passam).
## Pelo do Tlender na Skeletal Mesh (item 16, 2026-11-25)
- Import (`tlend_import.c` `import_hairs`): um hair cuja superfície tem armadura (`sk_owner`: modificador Armature ou presa a
  um osso) vai para o ator `SK_` da armadura; as guias passam pela `obj_world` da superfície, como os vértices da pele.
- Raiz na pele (`groom.c` `bind_skin`): ao crescer, cada fio guarda os 4 ossos e pesos (os bits do uv2) do vértice de pele
  mais perto da raiz (grade 32³ dos vértices); `bind_first` no buffer de dados, 2 palavras por fio. A cada quadro a paleta
  do Skeletal Mesh (`anim_skin_palette`, 3 vec4 por osso) vai depois da tabela de Grooms (`HAIR_PAL_AT`, até 65 536 vec4);
  `hair.vert` (`groom_skin`, hair.glsl) dobra o fio inteiro pela mistura dos ossos da raiz. O cabelo simulado segue o osso
  de maior peso das raízes (as linhas do passo × a paleta desse osso).
- `CGroom.visible` (padrão 1; níveis antigos leem 1): desligado, nem desenha nem simula; ator escondido no jogo idem;
  Details "Visible" e Set Property by Name "Visible" (`props_table.inc`, `gen_props.py` EXTRA_BITS).
- Contorno (item 17 parte 2): o cabelo do ator selecionado entra no mask (`mask_hair.frag` com o `hair.vert`,
  `groom_draw_mask`, o id do ator no pad).
- Teste `groom_skin` (fixture `hair_skinned.tlend`: armadura Rig, ação Spin no osso Arm, a peça Piece presa a ele, 8 guias):
  Groom no ator SK_, fios dobrados pelos ossos, o osso girando (0,32), Visible desligado sem cabelo e de volta, o cabelo no
  mask com o ator selecionado (39 quadros) e fora dele desligado (0). `hair`, `hair_dynamic`, `hair_crowd`,
  `import_tlend_hair` passam.