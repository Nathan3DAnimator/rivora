# Visão distante como em RDR2 (Q21, 2026-11-18)

Quilômetros de paisagem bonitos e baratos: a sombra do terreno além das sombras virtuais do sol, impostores octaedrais
no lugar das malhas distantes e a neblina distante na cor do céu (a perspectiva aérea física já existia: luz.md). Código:
`src/farview.c/.h` (no `re_render`), `shaders/farview.glsl`, `impostor.glsl`, `farshadow.comp`, `impostor_bake.vert/.frag`;
cena e testes em `src/farview_test.c`.

## Sombra distante (a sombra do terreno)
- As sombras virtuais do sol (luz.md) cobrem metade do lado do último nível em volta da câmera (Épico ±1 km, Baixo ±64 m) e
  só desenham os casters dentro desse alcance. Além disso tudo ficava aceso: montanhas sem sombra umas nas outras.
- **Heightfield**: as alturas das Landscapes (`landscape_height`, também o Procedural World por baixo) numa grade quadrada
  sobre a extensão delas (`landscape_bounds`), `far_res` texels por lado (Baixo 512, Médio a Épico 1024, Cinemático 2048:
  7,8 m por texel em 8 km). Feito na CPU em paralelo (`re_parallel_for`, 1024²: 5–10 ms) quando `landscape_generation()`
  muda (criada, apagada, movida, escultura, importação), copiado para um buffer da GPU (`far_buf`).
- **Horizonte** (`farshadow.comp`, só quando o sol gira mais de ~0,05° ou o heightfield muda): para cada texel, marcha em
  direção ao sol pelo azimute dele (96 passos crescendo 9%: o primeiro um texel, o campo inteiro depois de ~90), alturas
  bilineares; guarda a maior subida (tan do ângulo) e a distância dela em duas palavras: a subida **mais perto que o corte**
  (um quarto do lado do último nível virtual) e a **de depois**. 1024²: 1,4 ms de GPU por passe (timestamps 32–33).
- **Leitura** (`farview.glsl far_terrain_shadow`, chamada por `vsm.glsl vsm_shadow`): o disco do sol (0,27° + o borrão de
  meio texel visto dali) contra o horizonte visto do ponto (a subida menos a altura do ponto sobre o chão / a distância:
  uma copa ou uma torre vê mais longe que o chão embaixo), bilinear em 4 texels. Além dos níveis virtuais: as duas palavras.
  **Dentro** deles: o mínimo do mapa com a palavra **de depois do corte** (o vale ao pé da serra no pôr do sol, que o mapa
  virtual não desenha por estar fora do alcance). Nos últimos 10% do último nível o mapa passa para a sombra distante
  inteira (antes passava para "aceso").
- Detalhes da luz direcional: **Far Shadow** (liga) e **Far Shadow Strength** (0..1, `GpuFrame.far_g.x`).
  `farview_shadow_at` é o gêmeo na CPU (o teste confere a cobertura).

## Impostores octaedrais (HLOD de alta qualidade)
- **Quem ganha**: `render.c instance_of` anota cada instância parada de uma malha importada, opaca ou Masked, sem esqueleto
  (`farview_note_instance`: malha, material, planta?). Uma malha com 4 instâncias do mesmo material (ou de Foliage) e 200+
  triângulos é candidata; o ator Foliage com **Impostors** desligado tira a malha da lista. Até 40 impostores com atlas de
  1024 (96 com 512). O material das outras instâncias tem que ser o mesmo (a chave é o slot do material + 1; 0xFFFFFFFF:
  sem material, a cor da instância).
- **Bake** (`farview.c bake_one`, um por quadro, numa submissão própria): o LOD 0 desenhado de 8×8 direções do hemisfério de
  cima (hemi-octaédrico), ortográfico na esfera envolvente, em dois alvos RGBA8 1024² (Baixo/Médio 512): cor base (sRGB, como
  o assado do material) + alfa; normal no espaço da malha (octaédrica) + profundidade ao longo da vista + rugosidade. Os
  dois **pré-multiplicados pela cobertura** (limpos em 0): os mips e o bilinear tiram a média só do que existe, e a leitura
  divide de volta (sem franja escura nas folhas). Copiados para duas texturas do conjunto bindless dos materiais
  (`mat_tex_new`, mips por blits). 1–4 ms por malha.
- **Tabela** (`imp_tab`, um uvec4 por slot de malha): chave do material, as duas texturas (16 bits cada), N, e as meias
  arestas da caixa da malha sobre o raio (10 bits cada).
- **Troca** (`cull.comp` passe 0): instância parada cujo raio na tela está abaixo de `impostor_px` (Cinemático 16, Épico 20,
  Alto 24, Médio 28, Baixo 32) e cuja chave bate com a tabela → o desenho do **cartão** (uma malha de 4 vértices registrada
  depois das importadas, `stage_card`, fora das listas do editor: um draw para todos os impostores). A sombra dela continua a
  malha no LOD mais grosso (o mapa virtual guarda um caster sólido). Vai sempre para a lista do pré-passe (alfa).
- **Cartão** (`mesh.vert`): de frente para a câmera no centro da esfera, do tamanho da **caixa da malha vista daquele lado**
  (as meias arestas da tabela projetadas nos eixos do cartão: o quadrado da esfera de uma árvore era 60% ar, e o pré-passe
  pagava para recortá-lo). `o_instance` com o bit 31 (cartão), `o_uvs.xy` o meio do UV2.
- **Leitura** (`impostor.glsl imp_card`): o ponto do cartão e a vista no espaço unitário da esfera; as três vistas do
  triângulo da grade em volta da direção, cada uma lida onde o raio cruza o plano dela, e um passo de paralaxe pela
  profundidade; cor, normal (malha → mundo pelos cofatores), rugosidade e o **ponto mostrado** (`g_pw` no `mesh.frag`: a
  sombra do sol, os raios, a perspectiva aérea e a neblina usam o ponto da superfície, não o do cartão no meio da árvore,
  que ficava dentro da sombra da própria copa). O pré-passe (`masked.frag`) lê só o alfa dos três planos (3 leituras); a
  cor 12. Mip pela derivada do ponto na esfera, com o viés do TSR.
- **Sem estouro**: o cartão é iluminado pelo mesmo `mesh.frag` (GI, céu, sombra no ponto mostrado), e o TSR mistura a troca
  em alguns quadros. Medido: a troca muda 0,065–0,13% dos pixels da vista em mais de 16/255, além da deriva da própria
  imagem (teste `vista_pop`).
- **HLOD**: os proxies do World Partition (mundos-grandes.md) são instâncias da mesma malha: ganham o impostor sozinhos; as
  regiões fundidas (uma instância cada) continuam malhas.

## Neblina na cor do céu
- `weather.glsl fog_far_light`: a luz ambiente da neblina analítica (a parte além dos froxels, a dos quilômetros) deixa de ser
  o cinza do céu inteiro (a média do SH para cima e para baixo) e passa a ser a luz do céu **do lado para onde a vista olha**
  (o SH da luz do céu no horizonte à frente): mais clara e quente para o lado do sol, mais azul do outro. Longe a neblina fica
  da cor do céu atrás dela e da perspectiva aérea. Detalhes da Exponential Height Fog: **Sky Atmosphere Ambient** (0..1,
  padrão 1; `far_g.z` = preset × componente). Vale em todo shader que lê a neblina analítica.
- Testado e descartado: a linha do horizonte da LUT do céu (`sky_radiance`, depois só a linha do horizonte pelo azimute)
  custava 16 bytes de spill no `mesh_rt`/`mesh_rt_t` (o passe principal com raios). O SH já está nos registradores.

## TSR no longe
- Sem mudança no `tsr.comp`. Os cartões dão ao TSR um sinal pré-filtrado (o mip do atlas) no lugar das folhas recortadas a
  1–3 km. Voando sobre a floresta escondido, o vai-e-volta da sonda foi 0,07–0,13% da vista com os impostores contra 1,10%
  com as malhas. Visível no 2º monitor a mesma sonda variou de 0,006% a 7% entre rodadas, com e sem impostores (outras sessões
  usando a GPU ao mesmo tempo): por isso o `vista_walk` só mede o custo e registra o número.

## Custo no resto da engine (registradores)
- O ramo do cartão fica só nas variantes **Masked** do `mesh.frag` (`#ifdef RE_MASKED`: só os runs Masked desenham o cartão);
  nas opacas `i_card` é `false` e o código é o de antes. Com ele em todas, o `mesh_rt` foi de 16 para 48 bytes de spill.
- O ponto mostrado pelo cartão é **um float** (`g_pt`, a distância além do cartão ao longo do raio da vista), não um vec3.
- A sombra distante tem **uma** chamada no `vsm_shadow`, e lê o horizonte interpolado (os 4 texels misturados, o disco do sol
  avaliado uma vez).
- O cartão só vira Masked (`R.mesh_masked`) quando existe um impostor: o `any_masked` liga os pipelines dos raios de sombra
  Masked no passe principal de qualquer cena (`skin` +0,6 ms quando o cartão sozinho ligava).
- Resultado: `--scene skin --no-spill` igual à base (`mesh_rt` e `mesh_rt_t` 16 bytes; passe principal 1,99 contra 1,97 ms).

## Bits, formatos e números
- `GpuFrame` +64 B depois do `tess_u`: `far_buf`, `imp_tab` (endereços), `far_u[4]` (flags `FAR_SHADOW` 1, `FAR_IMP` 2,
  `FAR_FOG_SKY` 4; o draw do cartão = slot×4; o 1º vértice dele; o lado do heightfield), `far_f[4]` (origem xy, texel cm, a
  troca em px), `far_g[4]` (força, elevação do sol em rad, a parte do céu na neblina, -). `_Static_assert` com `+ 64` e o
  alinhamento de `far_buf`.
- `TS_PER_FRAME` 32 → 34: 32–33 o passe do horizonte (só quando roda).
- Bit 31 do `o_instance` do `mesh.vert` (cartão): `mesh.frag`/`masked.frag` leem `i_instance_raw` e mascaram.
- `GiSettings` (no fim): `far_shadow`, `far_res`, `impostors`, `impostor_px`, `impostor_res`, `fog_sky`. cvars
  `r.FarShadow`, `r.FarShadow.Resolution`, `r.Impostors`, `r.Impostors.ScreenSize`, `r.Impostors.Resolution`,
  `r.Fog.SkyAtmosphere`. Ambiente: `REALENGINE_FAR_SHADOW=0`, `REALENGINE_IMPOSTORS=0`, `REALENGINE_IMPOSTOR_PX=<px>`.
- Componentes crescidos no fim: `CLight.far_shadow`, `far_shadow_strength`; `CFog.sky_atmosphere_ambient`;
  `CFoliage.impostors` (padrões do ECS: ligados; níveis antigos leem os padrões).
- `material.h`: `mat_tex_new`, `mat_tex_image`, `mat_tex_mips_in`, `mat_tex_release`. `landscape.h`: `landscape_generation`.
- Debug Viewer > **Distant View**; linha `Distant view:` no `--bench`. Sem nós de Blueprint (Set Property by Name e as cvars).

## Medidas (RTX 3060, 2560×1417, escondido, 5 s; a cena `vista`: 8×8 km, 36 529 árvores de 872 triângulos, 3000 rochas
de 1280, sol a 9°, o mirante a 320 m olhando 8 km)
| Preset | fps | GPU média / mínima (ms) | passe principal mín. | troca |
|---|---|---|---|---|
| Baixo | 268–270 | 3,71–3,73 / 3,58–3,59 | 2,28–2,29 | 32 px, atlas 512 |
| Médio | 189–194 | 5,14–5,28 / 4,94–4,97 | 3,32–3,35 | 28 px, 512 |
| Alto | 168–176 | 5,68–5,96 / 5,57–5,60 | 3,72–3,73 | 24 px, 1024 |
| Épico | 156–160 | 6,25–6,39 / 6,03–6,05 | 4,17–4,18 | 20 px, 1024 |
(duas rodadas intercaladas, a versão final; outras sessões usavam a GPU em parte do dia: as médias oscilam, as mínimas não)
- Épico **sem impostores** (`REALENGINE_IMPOSTORS=0`): 34 fps, GPU 29,1 ms (passe principal 14,8 + pré-passe 10,8). Com eles:
  **4,6× mais rápido** (156–160 fps). A troca a 40 ou 12 px muda pouco (147 / 140 fps, medidos antes dos últimos ajustes): o resto é o terreno e o sombreamento.
- Primeira versão dos cartões (o quadrado da esfera, 9 leituras no pré-passe): Épico 57 fps (passe principal 11,5 ms); com o
  cartão na caixa da malha e o pré-passe de 3 leituras: 144; com a sombra distante interpolada e o cartão Masked só com impostores: 156–160.
- **Sombra distante**: horizonte 1,4 ms de GPU por passe em 1024² (0,17 ms em 512²), só quando o sol gira. Leitura: Épico
  165 fps sem ela contra 156–160 com ela (GPU mínima 5,84 contra 6,03–6,05 ms: ~0,2 ms; 4 alturas + 8 palavras de horizonte
  interpoladas por pixel além dos níveis virtuais, 4 + 4 dentro).
  Heightfield na CPU 5–10 ms (uma vez). Cobertura na cena: 1831 de 4096 pontos do chão à sombra do terreno, 1490 acesos.
- Bake dos impostores: árvore 0,8–4 ms, rocha 1–5 ms; atlas 21 MB (1024) ou 5 MB (512) os dois.
- Sob validação, visível (`ctest`): `vista` 140–147 fps, `vista_low` 238–247, `vista_pop` 138–146, `vista_walk` 132–143.
- Abaixo dos 200 fps a partir do Médio (o Médio fica em 189–194). Próximos passos: LOD por bloco do Landscape (2 M triângulos desenhados), o custo da
  cena com 40 mil instâncias ("cena (TLAS, culling)" ~1 ms), a GI das sondas (0,4–0,6 ms).

## Testes (área `visao-distante`)
- `vista` (Épico, validação, `--min-fps 100`): sombra distante ligada e coberta, os dois impostores assados.
- `vista_low` (Baixo, `--min-fps 170`).
- `vista_pop` (visual, no kit rápido): três shots com a câmera parada (malhas, malhas de novo, cartões a 2,5× a troca): o que a
  troca muda além da deriva da própria vista, ≤ 0,4% dos pixels acima de 16 (medido 0,065–0,13%).
- `vista_walk` (voo a 20 m/s sobre a floresta, `--min-fps 90`): os cartões virando e trocando a cada quadro.

## Falta
- Cache do bake em disco; atlas comprimidos (BC7); impostor de malha com materiais diferentes por instância (hoje o
  primeiro material visto) e da escala de UV da instância.
- Transição com dither entre malha e cartão (hoje a troca é seca e o TSR a mistura); o cartão sem profundidade por pixel
  (corta no terreno inclinado na frente).
- Sombra distante das malhas (hoje só o terreno) e na neblina volumétrica, nas sondas e no cache.
- LOD por bloco do Landscape com costura (o custo principal da cena).

## Quem ganha impostor (merge do lote 7, 18/11)
- Antes: toda malha parada com >= 4 instâncias e >= 200 triângulos. No `demo_fighting` (arena pequena) isso assava 12
  impostores de alças e trincos de 0,1 m, e a simples presença de um impostor liga os pipelines Masked no passe principal:
  GPU mínima 4,1 → 4,9 ms (main 2,6 → 3,4).
- Agora (`farview.c`): plantas (Foliage, vento) sempre; os outros só quando a troca pelo cartão a 24 px (referência fixa,
  ~1440p a 90°) fica a 60 m ou mais (`FARV_MIN_SWITCH_CM`, raio ≳ 1,1 m) **e** a cena (caixa dos centros das instâncias
  paradas) é maior que essa distância. `farview_note_instance` recebe a caixa da instância.
- Depois: `demo_fighting` sem impostores, GPU mínima 4,31-4,39 ms contra 4,24-4,30 da base (3 pares alternados); `vista`
  mantém árvore (9,3 m) e pedra (1,6 m, troca a ~85 m).
