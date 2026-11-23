# Luz física (unidades, câmera, céu, sombras)

Pedido: unidades físicas e um sistema de luz realista, com a luz indireta junto, bem otimizado e de alta qualidade.
Tudo que ilumina (passe principal, sondas, bake, path tracer) usa as mesmas luzes, o mesmo céu e as mesmas sombras:
a GI ([gi.md](gi.md)) continua batendo com o Path Tracing (conferido lado a lado no nível padrão).

## Unidades (como a Unreal)
| Fonte | Unidade | Padrão |
|---|---|---|
| Directional | lux **no topo da atmosfera** (o chão recebe vezes a transmitância: ~0,85 ao meio-dia, vermelho no pôr do sol) | 120 000 lux |
| Point / Spot / Rect | candela (lux a 1 m; cai com o quadrado da distância em metros) | 1 500 / 3 000 / 1 500 cd |
| Sky Light | escala (1 = o céu físico) | 1 |
| Emissive | nits (cd/m²) | — |
`.rlevel` v2. Os v1 abrem convertidos (luzes e emissive ×20 000, o sol ×24 000: a mesma imagem que a exposição 1
dava); o `.rlight` v3 guarda a luz vezes 1/256 (half float) e os v1/v2 abrem convertidos (×78,125). Conferido:
o nível v1 convertido mede 10 827 nits de média contra 11 009 do padrão novo.

## Câmera física e pré-exposição
- Exposição = 2^compensação / (1,2 · 2^EV100) (ISO 100). Manual: o EV100 (15 sol pleno, 12 nublado, 7 interior,
  −2 luar). Automática (padrão): EV100 = log2(L·100/12,5) da luminância média entre os percentis 10 e 90, entre
  EV100 −4 e 18, adaptação 3/s para o claro e 1,5/s para o escuro. Compensação padrão +1 (como a Unreal).
- **Pré-exposição**: os shaders que iluminam multiplicam pela exposição do frame anterior (lida da GPU) antes de
  gravar no alvo RGBA16F, para 100 000 lux caberem em half float; o composite divide por ela e aplica a nova. Sondas
  e lightmaps guardam E/π vezes 1/256 (`GI_STORE`). Saídas limitadas a 60 000 (o disco do sol).

## Céu físico (`atmosphere.c`, `shaders/atmosphere.*`)
Atmosfera da Terra de Hillaire 2020: Rayleigh, Mie (g 0,8) e ozônio. LUTs na GPU para um sol de 1 lux:
transmitância 256x64 e espalhamento múltiplo 32x32 (uma vez), céu visto da altitude da câmera 192x108 e o SH do Sky
Light (só quando o sol gira ou a câmera sobe). Todo shader que vê o céu amostra a mesma LUT (céu de fundo, reflexos,
raios das sondas, bake, path tracer); o disco do sol (0,27°, escurecimento do limbo) vem com a transmitância. A
transmitância para cada luz direcional também é calculada na CPU (o mesmo modelo) e entra na radiância dela.
Sem luz direcional: céu preto (noite).

**Perspectiva aérea** (o ar entre a câmera e a cena): volume de 32x32x32 froxels sobre a vista (um atlas 1024x32),
fatias quadráticas na distância até 64 km, por froxel a luz espalhada (Rayleigh + Mie + múltiplo, para 1 lux de sol)
e a transmitância; refeito a cada frame (a câmera anda), terra plana em volta da câmera (a altitude segue a
direção, nunca abaixo do chão). Aplicado no passe principal e no ponto primário do path tracer (conferido lado a
lado). Settings > Aerial Perspective Scale (como a View Distance Scale da Unreal): 1 é o físico (só aparece com
quilômetros), 0 desliga, 300 deixa a névoa visível na escala do nível padrão.

## Sombras
- **Por raio** (Médio para cima, Settings > Ray Traced Shadows): toda luz com Cast Shadows, inclusive point/spot/rect, no
  passe principal: um raio exato por luz (sem ruído, sem acne, sem limite de distância); com Source Radius (esfera)
  ou rect, 4 (Epic 8) raios no disco/retângulo em espiral de Vogel girada por pixel (IGN): penumbra que alarga com a
  distância ao bloqueador.
- **Sol: sombras virtuais** (todas as escalas, ver "Sombras virtuais do sol" abaixo); do Médio para cima as outras
  luzes continuam por raio. **Baixo e GPU sem RT: mapas de sombra das luzes locais** (ver "Sombras das luzes locais por
  mapa" abaixo).
- A GI e o bake respeitam Cast Shadows (sem raio de sombra para quem não projeta).

## Medidas (RTX 3060, 2560x1417, viewport ~2100x1350, MSAA 4x, nível padrão)
| | ms GPU |
|---|---|
| Atmosfera (LUT do céu + SH), só quando o sol/altitude muda | 0,05 |
| Exposição (histograma + adaptação) | 0,03 |
| Perspectiva aérea (froxels 0,03 + aplicação no passe principal) | ~0,05 |
| Sombras por raio (sol + 1 point, 1 raio cada) | ~0,19 |
| Frame High (sombras por raio, sondas, reflexos) | 1,86 → 515 fps |
| Frame Medium (`--gi probes`) | 1,66 → 575 fps |
| Nível com lightmaps + volume | 1,45 → 655 fps; sem RT 0,79 → 1144 fps |
| Build Lighting Production | 0,68 s |

## Milhões de luzes (`lights.c`, `shaders/lights.glsl`, `light_grid.comp`)
- **Buffer persistente na GPU**: cada luz local (point, spot, rect) num slot por entidade (o mesmo índice da cena),
  `GpuLight` de 80 B, slot vazio = bytes 0xFF. Só o que muda é enviado: os slots do log de mudanças do ECS, a seleção
  (o Details edita no lugar: comparada com a última cópia) e as Movable/Rotating (comparadas a cada frame).
  Directional e Sky ficam numa lista curta (`lights_globals`) lida a cada frame — antes um laço sobre todas as luzes.
- **Grade de luzes no mundo** (clipmap de 3 níveis, 64×64×16 células de 4 m / 16 m / 64 m em volta da câmera: 256 m,
  1 km, 4 km), refeita na GPU só quando uma luz muda ou a câmera cruza uma célula de 4 m: contar (uma luz por thread,
  as células dela divididas pelo subgrupo inteiro), alocar (um atômico por workgroup), preencher. Até 8 M itens
  (32 MB). Uma luz mora no nível mais fino em que cobre até ~17 células por eixo e entra nos mais grossos enquanto o
  alcance for ≥ 1/4 da célula (LOD: luzes pequenas param de iluminar a 128 m / 512 m da câmera). Num ponto lê-se o
  nível mais fino que o contém (todas as luzes de lá) e, nos mais grossos, só as luzes "de casa" deles.
- **Quem usa**: o raster (`mesh.frag`), as sondas da GI, os reflexos e o path tracer (`direct_irradiance` no
  `rt.glsl`) — a mesma grade, então luz direta e indireta concordam. Conferido contra força bruta (todas as luzes por
  pixel): diferença < 1% em toda a tela.
- **Sombras**: com sombras por raio (Alto/Épico) até `shadow_budget` luzes com Cast Shadows por pixel (4 Alto, 8 Épico;
  o sol primeiro), as demais sem sombra. Sem RT (e no Baixo): o mapa do sol e os mapas das luzes locais (`lsm.c`).
- **Editor**: ícones das luzes em grade espacial na CPU (refeita quando o log muda); acima de 4096 atores-ícone só os
  a menos de 50 m aparecem (e são clicáveis). Projetar 1 milhão por frame deixava o editor a 1 fps.
- Limite conhecido: o Build Lighting usa a grade em volta da câmera — luzes locais a mais de ~1 km da câmera não
  entram no lightmap.

### Medidas (stress: 1 luz por 3×3 m, alcance 2–15 m: ~40 luzes alcançam cada ponto do chão)
| Cena | fps | GPU (ms) |
|---|---|---|
| Nível padrão (antes / depois) | 451 / 433–484 | principal 1,35 → 1,42 |
| 10 k luzes | 206 | GI 0,43 · principal 4,1 |
| 100 k luzes / com 10% e 100% projetando sombra | 195 / 185 / 175 | principal 4,3 / 4,5 / 4,5 |
| 1 M luzes (1,8 M itens na grade) | 165–170 | cena 0,17 · GI 0,45 · principal 4,6 |
| Reconstruir a grade (câmera andando), 1 M / 100 k luzes | — | 0,7 / 0,5 (antes do subgrupo: 2,6) |
| 100 k luzes sem RT / path tracing | 295 / 159 | — |
| 1 M objetos + 48 k luzes (limite de entidades do ECS) | 163 | principal 4,9 |

O custo dominante é sombrear ~40 luzes por pixel na tela inteira (caso extremo); jogos reais têm poucas por pixel.
Próximo passo para mais: amostragem estocástica de luzes (estilo MegaLights/ReSTIR) com denoise.
## Falta
- Nuvens volumétricas; neblina de altura (fog local) com luz das sondas.
- PCSS (penumbra pelo Source Radius) nos mapas das luzes locais; tecidos e partículas nesses mapas.
- Temperatura de cor (Kelvin) nas luzes; emissive com intensidade separada da cor; IES.
- Denoise/temporal curto para as sombras suaves com poucos raios (hoje padrão de pontilhado na penumbra).

## Céu, nuvens e neblina (2026-10-30; `weather.c`, `weather.comp`, `weather.glsl`)
Atores novos (Adicionar > Efeitos Visuais): **Exponential Height Fog** e **Volumetric Cloud** (componentes `CFog`,
`CCloud`: todo campo é dado, um Blueprint muda e vale no mesmo frame). **Predefinições de céu** (Clear Day, Morning,
Sunset, Cloudy, Overcast, Foggy, Night) ajustam sol, luz do céu, nuvens e neblina juntos (`--sky N`).
- **Nuvens**: panorama 1024 x 512 em volta da câmera (azimute x elevação, um quarto das linhas por frame), raymarch de
  48 passos na camada (Terra redonda), ruído Perlin-Worley 64³ gerado na GPU uma vez; cada cúmulo tem a altura que a
  força da célula permite; luz: o sol em 7 passos a partir de 30 m (a pele iluminada da nuvem), espalhamento múltiplo
  por oitavas (Hillaire) + transmissão difusa 1/(1+0,75τ(1−g)) (sem ela o fundo da nuvem ficava preto/azul), o céu
  limpo do LUT como ambiente (sem realimentação). **Sombra das nuvens** no chão: mapa 256² sobre 20 km, aplicada ao sol
  em todo lugar (pixel, cache, sondas, reflexos). A **luz do céu** (SH) é refeita com as nuvens: nublado fica cinza e
  sem sombras duras; os raios da GI que escapam veem as nuvens (`sky_clouds`).
- **Neblina**: densidade exponencial na altura; volumétrica em froxels 128 x 72 x 64 (fatias exponenciais até a
  Volumetric Distance): sol com raio de sombra (feixes de luz) e sombra das nuvens, céu, até 8 luzes locais da célula
  (halo de poste), integrada da frente para trás; além disso a neblina analítica. Sem história temporal: sem ghosting.
- Bug achado: `smoothstep(1, x, h)` (bordas invertidas) é indefinido em GLSL e zerava as nuvens na NVIDIA; outro
  igual no AO de contato também corrigido.
- Medidas (nível padrão, High, 2560×1417): 385–448 fps conforme a predefinição (passes do clima 0,4–1,1 ms; o céu
  limpo é o mais caro: raios longos sem nuvem para parar). Teste `weather_sky` (pôr do sol, validação, > 350 fps).

## Unidades à escolha e mais opções (2026-10-31)
- **Intensity Units** no Details (como a Unreal): Candelas, Lúmens, EV e **Watts (Blender)** para point/spot/rect; Lux e
  **W/m² (Blender)** para a directional. O valor continua guardado em cd / lux (Blueprints e `.rlevel` não mudam); o
  Details converte: lúmens = cd × ângulo sólido (point 4π, spot o cone externo 2π(1−cos), rect π), EV: cd = 2^EV / 8
  (a Unreal), Watts: lúmens a 683 lm/W (spot do Blender = point mascarado: 4π), W/m² × 683 = lux. Trocar a unidade mantém
  o brilho. Watts servem para as luzes vindas do Tlender ficarem iguais.
- **Use Temperature / Temperature** (K, 6500 = branco): locus de Planck (ajuste de Kang 2002) → sRGB linear, luminância 1;
  multiplica a cor (também o sol do céu). **Affects World** (desligada: não ilumina nada; o sky light também).
  **Indirect Lighting Intensity** (a parte da luz na GI: cache, sondas, bake, ricochete dos oclusores; o path tracer
  continua físico, `RT_PHYSICAL_LIGHTS`), **Specular Scale** (brilho especular no passe principal), **Volumetric
  Scattering Intensity** (a luz na neblina volumétrica). As três escalas vão nos bits livres do `GpuLight.type` (9–15,
  16–23, 24–31; `light_*_scale` no `common.glsl`): custo zero, `GpuLight` continua 80 B.
- `CLight` cresceu: o ECS agora lê componentes que **cresceram** (linhas na largura antiga, o resto com os padrões de
  `init_component`) — níveis antigos abrem sem subir versão. Blueprints antigos (bytes zerados) têm `ext = 0`: lights.c
  usa os padrões.
- `geometryShader` ligado no device (gl_PrimitiveID da visualização Triângulos).

## Nuvens mais realistas (2026-10-31)
Relato: as nuvens não pareciam reais (manchas lisas e borradas). Causas e mudanças (`weather.comp`, `weather.c`):
- **Resolução**: o panorama era 1024×512 para 360° (2,8 px/grau; a tela a 1440p tem ~28 px/grau: ampliado 10×). Agora
  3072×768 (~8,5 px/grau), 1/32 das linhas por quadro (o céu inteiro em ~0,1 s; o vento é lento).
- **Marcha**: 48 passos em até 50 km pulavam todo o detalhe; agora até 64 passos de no mínimo 80 m, **pulo de espaço
  vazio** (forma grossa, passo dobrado) e a marcha de luz até o sol com a forma grossa (sem o detalhe, como a Nubis).
- **Forma de cúmulo**: ladrilho de 4 km (nuvens de 0,5–1 km), base plana e nítida, **perfil em domo** (em cada altura só
  a parte da célula mais forte que (h/altura)^1,5: estreita para cima, topo redondo, sem "cogumelo"), altura pela cobertura.
- **Erosão de detalhe** em ~330 m e ~125 m com distorção de domínio: ondulado ("couve-flor") no topo, esfiapado na base.
- **Máscara de clima** (~16 km): as nuvens vêm em campos com céu limpo entre eles.
- **Cobertura correta**: o Perlin-Worley tende a valores altos; o `smoothstep(0,25..0,75)` saturava metade do céu em 1
  e "Clear Day" (0,15) ficava quase coberto. Agora `0,45..1,0`.
- **Luz**: lobo frontal mais forte (contorno prateado), beer-powder (Schneider), oitavas de espalhamento múltiplo mais
  fortes (nuvem espessa clara por dentro), ambiente do céu mais forte no topo e o chão iluminado na base.
- Custo (nível padrão, 1440p): clear 405 fps, pôr do sol 368, nublado 371 (antes 368–371). Teste `weather_sky`.
- Falta: tipos de nuvem por altitude (cirros), reprojeção temporal para mais passos, comparação com fotos de referência.

## Nuvens sem granulado (2026-10-31)
- O deslocamento do primeiro passo do raymarch era um hash fixo por pixel do panorama: o mesmo ruído todo quadro
  ("sal e pimenta" nas bordas). Agora ele anda pela razão áurea a cada atualização e o panorama **acumula** (¼ da
  atualização nova sobre as anteriores; as nuvens estão a quilômetros e o vento é lento: o histórico vale). Custo zero
  (nublado: 564 fps antes e depois).
- **Nuvens na resolução da tela** (`weather.comp` passo 6): ¼ da vista em cada eixo (imagem 960×540, a parte usada
  conforme a tela), 48 passos de no mínimo 25 m com deslocamento por pixel (gradiente intercalado) que anda pela razão
  áurea a cada quadro, sobre o histórico reprojetado pela vista do quadro anterior (só a direção: as nuvens estão a
  quilômetros), 10% por quadro; copiado para a imagem que o céu lê (`clouds_screen`). Reflexos, GI e luz do céu seguem no
  panorama. Custo (nublado, 1440p): 564 → 459 fps (+0,4 ms de GPU); pôr do sol 395 fps; testes passam.
- **Forma** (antes: "massinha"): 2ª oitava na forma (~1,5 km: contornos irregulares), 3ª oitava de erosão (~45 m, só
  nas bordas, onde aparece), erosão que morde mais onde a nuvem é fina, fiapos só na base (antes subiam e pareciam
  pendurados), limiar 0,40..0,95 (duas oitavas misturadas têm picos menores). O panorama (reflexos, GI, luz do céu) usa
  só a forma grossa; na tela a erosão vai até 12 km. Xadrez: metade dos pixels marchados por quadro (40 passos).
- Custo (1440p, sem validação): limpo 409, pôr do sol 376, nublado 397 fps. Com validação o `weather_sky` (pôr do sol
  olhando o horizonte, o pior caso) ficou em 330 fps: mínimo do teste 330 → 310. **Escalabilidade**: Baixo/Médio só o panorama,
  Alto/Épico as nuvens na tela (`GiSettings.screen_clouds`). Pôr do sol: Médio 442 fps, Alto 331 (`--scalability N` no
  editor, para medir).
- Falta: tipos de nuvem por altitude (cirros, estratos).

## Neblina sem vazar em lugar fechado (2026-11-02)
Froxels: a luz do céu multiplicada pela visibilidade de 2 raios para cima (35°, girados por froxel; 8% de piso). Neblina
analítica (além dos froxels, ou sem volumétrica) na frente de uma superfície: × `mix(0.1, 1, sqrt(GI/céu))` dela
(`fog_apply_occl`). Túnel do MyProject (Épico): brilho médio 5230 → 1637 nits, GPU 7,56 → 7,33 ms.

## Nuvens na tela sem piscar (2026-11-03)
Relatório #30 (RPG "piscando"): com a câmera parada, 0,46% de flicker no RPG Épico ao amanhecer e 3% da vista mudando > 2%
por quadro — quase tudo nas bordas das nuvens (desligar as nuvens na tela: 0,07%). Cada marcha pesava 15% sobre o
histórico: o ruído do deslocamento ficava. Agora o peso segue o tempo entre quadros: constante de 0,35 s
(`1 − exp(−2·dt/0,35)`, cada pixel marchado a cada 2 quadros; ~5% a 200 fps, ~17% a 60 fps), vindo de `render.c` em
`cloud_scr.z` (0 = sem histórico). Flicker 0,46% → 0,12%, vista mudando 3% → 0,06%; custo zero. As nuvens estão a
quilômetros e o vento é lento: 0,35 s não arrasta; girar a câmera reprojeta exato.

## Nuvens como as da Unreal 5.8 (sugestão #44, 2026-11-04)
Relato: "parecem massinha". Causas vistas nas capturas: todas as células com o mesmo domo liso, bordas em rampa suave,
a distorção forte do ruído desenhando ondas, o chão tingindo a base de marrom, e a marcha em ¼ da tela borrando.
- **Tipos de nuvem** (o 2º canal do mapa de clima da Unreal/Nubis, sem leitura a mais: o `.g` da mesma amostra):
  estratos (lâminas baixas, paredes retas, topo plano, erosão mais suave), cúmulos (domos), cumulonimbos (torres um
  pouco mais altas, mais largas em cima); céu fechado vira uma camada de estratos. Torres limitadas a 1,2× (1,55× custava
  ~1 ms: mais volume dentro da nuvem).
- **Bordas**: erosão "couve-flor" 1,45× mais funda nos cúmulos; a densidade sobe em `sqrt` depois da borda mordida
  (silhueta nítida, núcleo igual); extinção 1,4× (núcleos mais densos); distorção 1,6 → 0,9; o chão ilumina a base
  com metade (0,25 → 0,12).
- **Cirros** (`CCloud.cirrus`, Detalhes > Cirros, predefinições de céu: limpo 0,45, manhã 0,5, pôr do sol 0,6, nublado
  0,3, fechado/neblina 0): camada fina de gelo a ~9 km, em manchas e faixas, fios curvados esticados pelo vento com as
  pontas penteadas; luz do sol com lobo para frente (brilho em volta do sol) e do céu. Ruído de valor com hash inteiro
  (sem seno), calculado no passe das nuvens da tela (¼ da vista) e no panorama (reflexos, Baixo/Médio), atrás dos
  cúmulos. No `sky.frag` em resolução cheia custava ~1 ms; no passe das nuvens, nada mensurável.
- **Épico**: nuvens da tela em **meia** resolução (antes ¼), ¼ dos pixels marchados por quadro (células 2×2 em
  rodízio), memória de 0,5 s (cada pixel a cada 4 quadros); imagem 1920×1080. Alto continua em ¼.
- **Medidas** (RTX 3060, 1440p, olhando o céu; a CPU estava carregada por outra sessão, a GPU é a referência): Alto
  GPU 2,25 ms com cirros e tipos (2,29 ms sem eles); Épico +0,77 ms (as nuvens em meia resolução). Cintilação do
  `demo_horror_flicker` (Épico): 0,158-0,160% em meia resolução contra 0,168-0,252% em ¼ (esse teste oscila perto do
  limite de 0,2% pelo cache de GI). `weather_sky`, `play_flicker`, `demo_rpg_flicker` passam.

## Sombras virtuais do sol (2026-11-04; `vsm.c`, `shaders/vsm.glsl`)
A ideia das Virtual Shadow Maps da Unreal (um mapa de sombra enorme, desenhado só onde mudou e guardado entre os
quadros), adaptada ao que é barato aqui: em vez de páginas e tabela de páginas, um **clipmap em cache**.
- **Níveis**: quadrados concêntricos em volta da câmera, cada um uma camada de um array D32 (nível 0: 16 m, cada próximo
  o dobro; 2048 texels: 0,8 cm por texel nos pés, o último nível 2 km no Épico). Baixo 5 níveis de 1024 (128 m) com PCF
  3x3; Médio 6 (512 m), Alto 7 (1 km), Épico 8 (2 km), Cinemático 8 com mais amostras.
- **Cache**: cada nível guarda o quadro do seu desenho (centro, base do sol, faixa de profundidade) e o passe principal o
  lê com essa matriz. Um nível só é desenhado inteiro de novo quando a câmera andou 1/8 dele, o sol girou (> 0,03°), a
  faixa de profundidade da cena cresceu ou um estático mudou sem saber onde estava; orçamento de níveis inteiros por
  quadro (1 no Baixo/Médio, 2 do Alto em diante, os mais finos primeiro; níveis novos sempre).
- **Retângulos sujos**: um ator que se move (dinâmicos, partes de personagem, estático arrastado no editor: caixa antiga e
  nova; multidões: uma caixa por agente, juntadas em até 24) limpa e redesenha só o seu retângulo em cada nível. O
  vertex shader descarta toda instância cuja esfera não toca o retângulo (antes de ler o vértice): o custo é o dos
  casters do retângulo. O corte não tira os casters cuja sombra está fora da vista (`GI_VSM`): o cache vale para
  qualquer direção da câmera. Plantas desenhadas em repouso (sem vento) para o cache não mudar com o tempo.
- **Filtro (PCSS)**: busca de bloqueadores com 4 `textureGather` (16 profundidades); nenhum: luz; todos e penumbra
  pequena: sombra (sem filtro). Na penumbra, um disco de Vogel fixo (sem ruído) de comparações bilineares, raio pela
  distância ao bloqueador × tan(meio ângulo do sol, 0,5357° como o Source Angle da Unreal), amostras pela área da
  penumbra (6 até 12/16/24/32). Mistura com o próximo nível nos 10% da borda.
- No Alto/Épico o sol fica nas sombras virtuais (Settings > Sombras virtuais (sol)); desligado, o sol volta ao raio.
  `REALENGINE_VSM=0` faz o mesmo para comparar.

Medidas (RTX 3060, 1440p, GPU mínima; GPU dividida com outros apps, médias em várias rodadas):
| Cena | Antes | Sombras virtuais |
|---|---|---|
| Vila RPG, Médio (mapa 2048 único, 12 cm/texel) | main 3,16 + mapa 0,18 ms | main 3,09–3,29 + 0 ms (cache) |
| Vila RPG, Alto (sol por 1 raio, sombra dura) | main 4,73 ms | main 4,90 ms (penumbra suave) |
| `--bp-idle` (personagem animado, Médio) | — | 6 retângulos/quadro, 0,053 ms; cintilação 0,076–0,084% |
| Demo RPG (autoplay, Médio: jogador e NPCs andando) | mapa 0,18 ms | 1,0 ms → **0,08 ms** com a separação estático/dinâmico; 165 fps |
| `anim_crowd` (1000 personagens animados) | — | CPU de 19,5 → 6 ms (grade de caixas sujas); 109 fps (meta 80) |

**Separação estático/dinâmico** (como a da Unreal): cada nível tem uma camada só com os casters estáticos. Um nível
desenhado inteiro desenha os estáticos nela, copia para o nível e desenha os dinâmicos por cima; um retângulo sujo só copia
os estáticos de volta e desenha os dinâmicos (nesses quadros o corte põe só os dinâmicos na lista de sombra:
`GI_VSM_DYN`). Memória: o dobro (Médio 6 × 2 × 16 MB). Um estático que muda (o editor o arrasta) faz os níveis sobre ele
serem desenhados inteiros na vez deles. As caixas sujas caem numa grade de 8 m (tabela hash, O(1) por caixa).

## Nuvens do Tlender (2026-11-04; `cloud_noise.c`, `weather.c`, `shaders/weather.comp`)
Refeitas do zero com o modelo do Tlender (`../tlender/src/sky_clouds.c`, copiado e adaptado para a GPU), como pedido:
- **Ruído feito na CPU uma vez** (o mesmo código do Tlender, em paralelo, ~90–170 ms na abertura): forma 64³ RGBA16
  (Perlin-Worley + Worley fbm 4/8/16), detalhe 32³ RGBA8 (Worley fbm 2/4/8), ambos texturas 3D com repeat (o atlas 2D
  de antes lia duas fatias); dos valores da forma, os **limiares de cobertura por quantis** (65) e as **médias
  pré-filtradas** por cobertura × gradiente de altura (17×17), num buffer (`GpuFrame.cloud_tables`).
- **Mapa de cobertura** (o `weather()` do Tlender, value-noise fbm ×1/×2,3/×5,1): assado numa textura 1024² periódica
  (10, 23 e 51 células: sem emenda; 220 km até repetir). Analítico custava 12 hashes por amostra × 7 por passo.
- **Densidade** como a do Tlender: cobertura local = a do céu ± o mapa (×2,5), base que sobe sob um céu fechado,
  gradiente de altura (bases redondas, topos afinando), limiar pelo quantil, erosão pelo detalhe (fiapos embaixo,
  couve-flor em cima), média pré-filtrada quando o passo é longo (aqui 1/4 do passo: o histórico de quadros faz o
  resto da filtragem; com o passo inteiro as nuvens borravam).
- **Luz** como a do Tlender: 6 passos até o sol (40 m dobrando; detalhe nos 2 primeiros, a média sem textura do 4º em
  diante), Henyey-Greenstein de dois lobos, 3 oitavas de espalhamento múltiplo, powder, ambiente do céu de cima e do chão
  de baixo sombreado pelo deck (1/(1+0,15τ)), sol difundido por um deck grosso, integração por passo que conserva
  energia, névoa do horizonte (20 km) e a média do céu nos últimos ~2,3°. A luz comum a todas as direções (16 leituras
  do LUT do céu) sai uma vez por quadro (passada 7), não por pixel.
- Passos: panorama 40 (Baixo/Médio: é o céu visto), tela 36 no Alto e 48 no Épico, 1/4 dos pixels por quadro com o
  histórico reprojetado (o Alto marchava metade); ampliação Catmull-Rom (a bilinear borrava as bordas).
- `CCloud.density` agora é a extinção do Tlender em 1/m (0,045 = densidade 1); `REALENGINE_NO_CLOUDS=1` desliga para
  comparar.

Medidas (RTX 3060, 1440p, `--sky 3` olhando para cima; GPU dividida com outros apps): custo das nuvens no passe
"GPU scene" (com − sem nuvens) Médio ~0,33 ms, Alto ~0,7 ms, Épico ~1,8 ms. Teste `weather_sky` (Médio) 305–348 fps.

## GI e reflexos: o chão longe "molhado" (2026-11-04, relato do usuário)
Na vila, a grama longe parecia brilhante: (1) o mapa de rugosidade do Grass004 tem média 0,27 (a demo agora o
multiplica por 3); (2) o raio de reflexo das superfícies ásperas colava a imagem nítida do que acertava (troncos e
postes espelhados na grama) — agora o acerto pesa menos com a rugosidade e acima de 0,7 nem se traça; (3)
anti-aliasing especular (Kaplanyan e Hoffman 2016): a variação do normal no pixel alarga o lobo (sem granulado).

## Sombras do sol nos personagens: o híbrido mapa + raio (2026-11-06)
Pedido do usuário: no jogo, com personagens, sombras feias, com rastro e pouco responsivas. Reproduzido no MyProject
dele (Delights de cabelo de cards, sol a 20° de elevação, Épico com RT), filmando o jogo com os personagens animados.
- **O defeito:** a sombra do sol do cabelo saía como um borrão de 5–6 cm com degraus de texel, que ligavam e desligavam
  quando o cabelo balançava.
  - Com o sol baixo, um texel do nível 0 (0,8 cm) vira ~2,3 cm no chão.
  - Os fios recortados pelo alfa têm o tamanho de um texel.
  - O raio mínimo do filtro (1,5 texel) mais os degraus borram o resto.
  - Subir o raio mínimo para 3 texels não muda nada. Dobrar a resolução custaria ~1 GB.
  - A referência por raio (`REALENGINE_VSM=0`) mostra as silhuetas nítidas, com os fios.
- **O híbrido** (`vsm.glsl`, `mesh.frag`, `GI_SUN_HYBRID`; Alto/Épico com sombras por raio): a busca de bloqueadores da
  VSM decide o que o mapa sabe.
  - Nenhum bloqueador em volta: luz.
  - Bloqueador longe (penumbra acima de 2,5 texels): o filtro PCSS de antes, a penumbra larga e macia.
  - Bloqueador perto (penumbra de até 2,5 texels: a borda real é mais fina do que o mapa desenha): **um raio exato
    até o sol**, que vê os cards Masked pelo alfa. Vale também no miolo da sombra, onde o mapa juntava fios em blocos.
  - O raio substitui as amostras do filtro, por isso custa o mesmo.
  - `REALENGINE_SUN_HYBRID_OFF=1` desliga, para comparar.
- **Primeira tentativa, descartada:** raios só na faixa onde o filtro dava entre 0 e 1. Os degraus ficavam no miolo, e a
  oscilação subiu de 0,66% para 0,87%.
- **Medido** (RTX 3060, 1440p, mínimo da GPU, desligado × ligado):

| Cena | Desligado | Ligado |
|---|---|---|
| MyProject em Play | 11,45 ms | 11,41 ms |
| Vila RPG | 8,13 ms | 8,19 ms |
| `house` | 2,84 ms | 2,83 ms |

  Oscilação de ida e volta (pixels que trocam e voltam entre quadros seguidos, mesmos 12 quadros a 20 Hz):

| Variante | Oscilação |
|---|---|
| VSM antiga | 0,557% |
| Híbrido | 0,526% |
| Só raio (referência) | 0,475% |

- **Ferramentas de teste** (`main.c`), para filmar sombras em movimento:
  - `--shot-every N` com `--shot a.png` grava `a_001.png`… a cada N quadros da medida. Enquanto grava, o relógio do jogo
    anda 1/60 s por quadro: a sequência é um vídeo a 60 fps, embora cada PNG leve ~1 s.
  - `--play-eject` joga ejetado (F8), visto da `--cam`.
  - `--cam-move x,y,z` move a câmera do editor em cm/s.
  - O mapa de oscilação usado aqui é um script de comparação de quadros, fora do repositório.
  - (2026-11-07) Para repetir a vista de um projeto do usuário: `--project` com `--bench` imprime a câmera dele
    (`Camera: --cam ...`); `--sun pitch,yaw` troca o sol e `--add-floor N` põe o piso dos templates (escala N), os dois
    só na memória; `--list-actors` mostra também tipo, intensidade e sombra das luzes.

## Muitas luzes com sombra: amostragem estocástica (2026-11-06, otimização geral fase 2)
Antes, cada pixel calculava todas as luzes da sua célula da grade (até ~40), mas só as 8 primeiras com Cast Shadows
ganhavam raio (4 no Alto). As outras iluminavam sem sombra e a luz atravessava paredes. Cada luz de área custava 8 raios.
Agora é como o MegaLights da Unreal: os raios por pixel não crescem com o número de luzes.
- **`mesh.frag` (RT, `GI_SHADOW_SPLIT`):**
  - Toda luz local com sombra é somada sem sombra (E) e entra num reservatório ponderado pela luminância: dois slots,
    16 bits de um hash PCG cada, a semente muda por pixel e por quadro.
  - Depois do laço, as duas luzes sorteadas recebem os raios: 1/4 dos do preset por slot (Épico 2, Cinemático 4, Alto
    1). A mesma luz nos dois slots usa os mesmos raios.
  - A média das visibilidades estima a fração de E que chega. A esperança é exata, porque cada luz é sorteada na
    proporção da sua luz.
  - Sai no split como antes: E pré-exposto, mais a luminância vezes a visibilidade.
  - **Uma luz dura sozinha no pixel:** raio exato direto na cor, sem split (como antes).
  - Duas luzes exatas faziam o shader principal derramar 64 bytes em memória local, sem ganho medido; ficou só uma.
- **`shadow_split.comp`:** o bilateral por profundidade agora é de 7 taps (antes 13), porque um filtro mais largo só
  borrava as sombras duras.
  - **Histórico da visibilidade** (`ml_hist[2]`, R16F, bindings 61 e 62): o ponto é reprojetado pela câmera
    (profundidade → mundo → quadro anterior).
  - O histórico fica preso a ±0,25 da visibilidade filtrada deste quadro e é misturado com peso 0,1.
  - Sem isso, o filtro deixava manchas do seu tamanho, que o clip do TSR mantinha (cintilação 0,6% → 0,18%).
- **Liga** com qualquer luz local projetando sombra (`lights_cast_count`; antes só com luz de área).
  - O pool de descritores agora é contado da tabela de bindings.
  - `REALENGINE_NO_SHADOW_FILTER=1` volta ao caminho antigo.
  - `REALENGINE_SHADOW_BUDGET=<n>` traça n luzes por pixel. Com 64 e o filtro desligado, é a referência "todas
    traçadas".
- **TSR, junto:** em repouso, um histórico cortado pelo clip não perde mais a contagem de quadros, só em movimento.
  - O jitter tira linhas finas da caixa a cada quadro. Cortar a contagem subia o peso da amostra nova.
  - No `house_walk`, a cintilação era 0,10% (o teste oscilava no limite) contra 0,05% sem TSR; agora é 0,054%. O
    vai-e-volta caiu de 0,22% para 0,05%.
  - Sem rastro na figura que anda na casa assombrada.
- **Medido** (Épico, 2560×1417, RTX 3060, mínimo da GPU, cada rodada com a GPU livre):

| Cena | Antes | Depois |
|---|---|---|
| Estresse: 10 k luzes com sombra, noite, duras | 10,0 ms (só 8 por pixel com sombra) | 9,8 ms, todas |
| Idem, suaves (raio 20 cm) | 28,9 ms, 31 fps | **10,2 ms, 89 fps** |
| Referência (todas traçadas): duras / suaves | 14,7 / 71,0 ms | — |
| heavy | 13,05 ms | 12,77 ms |
| casa assombrada (Play) | 5,91 ms | 6,08 ms |
| RPG (Play) / house / clínica | 5,61 / 2,29 / 4,87 ms | 5,32 / 2,38 / 4,83 ms |

  - **Erro médio contra a referência** (luminância 0–255): duras 0,58 (antes 1,99), suaves 0,76 (antes 2,14).
  - **Cintilação do estresse:** 0,15% (antes 0,24%).
  - **Cenas de poucas luzes:** +0,1–0,2 ms, que é o filtro e o histórico rodando na tela toda.
- **Limite:** a razão é pela luminância. Com luzes de cores muito diferentes, a penumbra mistura as cores delas.
- **Teste `many_lights`:** o estresse suave com validação, ≥ 70 fps e cintilação ≤ 0,25%.

## Transição suave das sombras do sol (2026-11-08, pedido do usuário)
Relato: chegando perto, a sombra "aparece do nada" no peito da Delight. A causa no `vsm.glsl`: o híbrido decide raio ×
filtro por nível (penumbra ≤ 2,5 texels: raio). Um nível grosso, com texels grandes, pede o raio; o fino, mais vezes o
filtro. Na faixa de mistura entre os dois, se um nível pedia o raio, o raio **substituía o resultado inteiro**
(`sun_vis = raio`): ao sair da faixa a imagem trocava de uma vez de "raio" para "filtro do nível fino" (e vice-versa),
o salto que se vê ao se aproximar. Também a sombra terminava numa linha depois do último nível.
- Agora cada nível devolve a parte do mapa e a do raio (`g_vsm_ray_w`); o `mesh.frag` soma `raio × g_vsm_ray_w`: o raio
  entra e sai aos poucos com o peso do seu nível (fade in / fade out). Os últimos 10% do último nível somem para "aceso".
- Testado e descartado: faixa de 30% com smoothstep e uma rampa filtro→raio (2,5 a 4 texels): +0,18 ms de GPU (os pixels
  da rampa pagam filtro e raio) e mais cintilação; a faixa ficou em 10% (`VSM_BAND`).
- Custo: o mesmo da 0.23 (10 mil luzes, `many_lights`: GPU mínima 10,98 ms nas duas). A primeira versão, com dois
  globais por nível (parte do mapa e do raio), custava +0,6 ms nessa cena (registradores); a final volta ao jeito
  antigo (um nível que deixa o ponto para o raio devolve -1) e soma o peso dele. Cintilação: a casa
  (`house_movable`) foi de 0,086% para 0,104-0,119% dos pixels: a borda exata do raio agora entra misturada na faixa
  entre níveis (antes era tudo raio ou tudo filtro). Limites de `gi_house`/`gi_house_movable`/`gi_house_thin`/`shadow_filter` 0,12% e `house_walk`
  0,13% (este já oscilava 0,08-0,13%, nota do TSR).
- Medido de novo (2026-11-08, depois da neve, do tecido e da água): `gi_house` 0,089-0,091%, `gi_house_thin` 0,083%,
  `shadow_filter` 0,089-0,090%, `house_movable` 0,099-0,100%, `house_walk` 0,105-0,108% (estáveis em 3 rodadas). Limites
  apertados de volta: 0,10% nos três primeiros, 0,11% e 0,12% nos dois últimos. Faixa de 6% testada: pior (0,110-0,295%
  na `house_movable`, 0,118% no passeio): os 10% ficam.
- Os limites apertados não aguentaram a suíte inteira: lá a casa deu 0,107-0,110% e a `house_movable` 0,129% (sozinhos
  0,079-0,100%, o resto da suíte aquece a GPU e muda o ritmo). Voltaram a 0,12% (casa, casa fina, filtro) e 0,13%
  (`house_movable`, passeio): um teste que oscila no limite não acusa nada.
- Filmado (MyProject, sol atrás da câmera, câmera de 14 m a 1,5 m, 49 quadros): de perto igual à 0.23.

## Especular vazando: luz atravessando paredes (2026-11-08, plano de melhorias A7)
Cena `spec_leak`: dois cômodos separados por uma parede de 5 cm feita de dois cubos sobrepostos, com uma parede em T
atravessando; atrás dela uma lâmpada forte (esfera de 10 cm, 60 000 cd) a 30 cm e uma segunda luz; do lado escuro o chão
e a face da parede são metal liso. A câmera só vê o lado escuro. Medida nova: `--max-scene-lum` (o ponto mais claro da
sonda 64×32 de luminância da cena, sem exposição; o bench imprime "Scene luminance").
- **Medido** (1440p): Épico pico 1,2 nits, média 0,0003; Alto 0,78; **Médio pico 3174, média 315** (o lado escuro todo
  aceso). Desligar o filtro do split, subir o orçamento de raios (64) ou tmin 0,02 nos raios de visibilidade não muda o
  Épico: as suspeitas 1, 2 e 4 do plano não são a causa aqui.
- **Causa principal**: no Médio as luzes locais não tinham sombra (`rt_shadows` só do Alto para cima), mesmo numa GPU com
  RT, e o editor abre no Médio. Agora o Médio traça as sombras das luzes locais (o sol continua no mapa): média 315 →
  0,001 nits, pico 10. Custo (GPU mínima): house 1,586 → 1,778 ms, nível padrão 1,535 → 1,773, spec_leak 1,587 → 1,939.
- **O que sobra**:
  - Épico 1,2-1,7 nits e Médio ~10 nos cantos onde a parede de 5 cm encosta na parede externa (perto do chão): o cache
    de superfície (o texel da parede externa, que passa pelos dois cômodos, atravessa a junção). Some no path tracer.
  - Sem o cache (`--no-cache`, só sondas) o cômodo escuro inteiro clareia (pico 139): as sondas de 2 m vazam pela
    parede fina.
  - **Baixo e GPU sem RT**: as luzes locais ficavam sem sombra (o lado escuro aceso). Corrigido com os mapas das luzes
    locais (A1d / A7g do plano): ver "Sombras das luzes locais por mapa".
- Testes `spec_leak` (Médio, pico ≤ 30) e `spec_leak_epic` (Épico, pico ≤ 5). Artigo do Assistente `scalability`.

## Sombras das luzes locais por mapa (2026-11-09, plano de melhorias A7g / A1d; `lsm.c`, `shaders/lsm.glsl`)
No Baixo e nas GPUs sem `VK_KHR_ray_query` (em qualquer preset) as luzes locais não tinham sombra. Na `spec_leak`, o lado
escuro tinha pico de 3037 nits no Baixo e 3174 sem RT. Agora elas têm **mapas de sombra**. Do Médio para cima com RT
nada muda: fica o caminho por raio de hoje (`lsm_configure` recebe 0 luzes e o atlas vira um 1x1).
- **Atlas** D32 com duas camadas: os mapas e só os casters estáticos (a separação estático/dinâmico das sombras
  virtuais). A pontual desenha as 6 faces de um cubo; a rect e a spot com mais de 60° também. A spot desenha uma face.
  Cada face tem uma borda de 3 texels (a projeção cobre um pouco além dos 90°), então o filtro nunca lê fora dela.
- **Escolha por importância**, a cada quadro, entre as luzes com Cast Shadows cuja esfera toca a vista.
  - A nota é o tamanho na tela (alcance / distância, até 100) × a raiz quarta da intensidade.
  - Uma luz que já tem mapa ganha ×1,5, para não trocar à toa.
  - Uma que saiu da vista guarda o mapa enquanto houver lugar (nota ×0,05) e só é redesenhada quando volta.
  - O orçamento: Baixo 8 luzes num atlas de 2048 (faces até 512); Médio 16 em 4096 (até 1024); Alto 24; Épico 32.
  - A face tem metade do raio da luz na tela, em potência de 2, de 64 até o máximo. Uma luz mantém a sua resolução
    enquanto a nova ficar a 2× dela.
  - Se as faces não cabem no atlas, a resolução das menos importantes cai pela metade. No mínimo (64), a menos
    importante fica sem mapa.
- **Tiles numa quadtree buddy** (blocos de 64 até o atlas), persistentes: uma luz guarda os seus enquanto a resolução não
  muda. Os novos são alocados do maior para o menor. Se fragmentar, tudo é realocado e desenhado de novo.
- **Cache** (só redesenha o que mudou):
  - Luz nova ou realocada: todas as faces no mesmo quadro, fora do orçamento. Ela não projeta nada até ter desenho.
  - Luz que se moveu (posição, alcance, direção, cone): todas as faces de uma vez, dentro do orçamento de faces inteiras
    por quadro (Baixo 12, Médio 24, Alto 36, Épico 48; pelo menos uma luz por quadro). Até lá, a tabela guarda a luz
    como foi desenhada: a sombra fica certa, só mais velha.
  - Estático que mudou dentro de uma face (`shadows_dirty_static`, o mesmo das sombras virtuais): a face é desenhada
    inteira de novo.
  - Dinâmico que se moveu (`shadows_dirty_box`, caixa antiga e nova): a camada estática do tile é copiada de volta e só os
    dinâmicos são desenhados por cima.
  - Tudo (nível novo): todas as faces, dentro do orçamento.
- **Culling na GPU**:
  - O `cull.comp` (passe 0) põe na lista de sombra os casters dentro das esferas das luzes desenhadas no quadro (até 32).
    Quando a luz só tem faces dinâmicas, entram só os dinâmicos.
  - O `mesh.vert` (passe 1, bit 31 do `pad`) descarta, antes de ler o vértice, toda instância cuja esfera fica fora dos
    4 lados da face ou do alcance da luz.
  - O desenho tem 3 passos para todas as faces de uma vez: estáticos na camada estática (tiles limpos por
    `vkCmdClearAttachments`), cópia dos tiles para os mapas, dinâmicos por cima. Cada passo é um rendering só.
  - O culling é por instância. O dos clusters não foi ligado às faces.
- **Leitura** (`mesh.frag`, `lsm.glsl`):
  - O slot da luz acha o mapa numa tabela hash de 256 entradas (`LsmBuf`, copiada para a GPU junto com o frame).
  - A face é escolhida pelo maior eixo, como num cubemap. O filtro é uma grade 2x2 (Baixo) ou 3x3 de comparações
    bilineares, sem ruído.
  - Bias: o ponto sobe pela normal 1,5 texel daquela distância, mais o bias por inclinação do pipeline de sombra.
  - "Cast Shadows" desligado: a luz não entra na lista (`lights_cast_list`).
- **Variante `mesh_lsm`**: o código dos mapas leva o `mesh` (sem RT, o do Baixo) de 95 registradores sem spill para 80
  com 48 bytes de spill. O driver escolhe isso mesmo só com a busca na tabela, sem retorno antecipado e sem o filtro
  (testado). É o degrau de -3% que está em render.md.
  - Por isso o `mesh` continua sem os mapas. O render usa o `mesh_lsm` só nos quadros em que alguma luz tem mapa
    (`GI_LOCAL_SM`).
  - O `mesh_rt*` não muda (128 registradores, 16 bytes). O teste `shader_spill` não soma o `mesh_lsm`.
- **Ferramentas**:
  - `--shadow-lights N`: N pontuais com sombra dentro das malhas da cena.
  - `REALENGINE_LSM=0` desliga os mapas (a comparação). `REALENGINE_LSM_LIGHTS=<n>`: n luzes com mapa, atlas 4096.
  - `REALENGINE_LSM_REDRAW=1` redesenha tudo a cada quadro, dentro do orçamento (o custo do desenho).
    `REALENGINE_LSM_LOG=1` mostra o que foi desenhado em cada quadro.
  - O bench imprime "Local shadows": luzes, faces inteiras e dinâmicas, GPU do desenho (timestamps 18-19) e CPU.
- **Medida da luminância**: o `--max-scene-lum` agora ignora os 32 primeiros quadros. Quando o warm-up era engolido por um
  quadro lento, o bench começava cedo e os quadros 3-12 marcavam até ~2000 nits na `spec_leak`, com RT também. Era isso
  que fazia o teste oscilar.

Medido (RTX 3060, 2560×1417, GPU mínima em ms, a menor de 2 rodadas; outras 5 sessões usavam a GPU):

| Cena | Sem mapas: quadro / main | Com mapas: quadro / main | Lado escuro: pico / média (nits) |
|---|---|---|---|
| `spec_leak` Baixo | 1,041 / 0,672 | 1,060 / 0,701 | 3034 / 312 → **1,3 / 0,0005** |
| `spec_leak` sem RT (Médio) | 1,142 / 0,772 | 1,382 / 1,016 | 3174 / 315 → **1,2 / 0,0003** |
| `house` Baixo, só a spot da casa | 0,996 / 0,631 | 1,005 / 0,639 | — |
| `house` Baixo + 4 luzes | 1,126 / 0,716 | 1,195 / 0,781 | — |
| `house` Baixo + 16 (8 com mapa) | 1,399 / 0,927 | 1,553 / 1,084 | — |
| `house` Baixo + 64 (8 com mapa) | 2,333 / 1,740 | 2,845 / 2,255 (303 fps) | — |
| `house` sem RT (Médio) + 4 | 1,212 / 0,835 | 1,313 / 0,932 | — |
| `house` sem RT + 16 (16 com mapa) | 1,514 / 1,114 | 1,932 / 1,527 | — |
| `house` sem RT + 64 (16 com mapa) | 2,769 / 2,335 | 3,760 / 3,333 | — |
| `spec_leak` Baixo + 4 / 16 / 64 | 1,163 / 1,373 / 2,292 | 1,248 / 1,645 / 2,908 | — |

- **Desenho** (`REALENGINE_LSM_REDRAW=1`: o orçamento inteiro a cada quadro; GPU por quadro que desenha):
  - Baixo: 0,08 ms (casa + 4), 0,09 (+ 16), 0,15 (+ 64). Sem RT, Médio (faces até 1024): 0,24 / 0,49 / 0,52 ms.
  - Parado, nada é desenhado (0 faces em 2000 quadros).
  - O personagem do `--bp-idle` com 8 luzes no Baixo, à noite: ~4 faces dinâmicas por quadro, 0,04 ms de GPU.
- **CPU do `lsm_plan`**: 0,002 ms (1 luz), 0,01 (5), 0,035 (17), 0,12-0,13 ms (65 luzes).
- **Cintilação** (`house` Baixo + 16 luzes): 0,076% com os mapas, 0,066% sem. A `house` sozinha no Baixo dá 4,6% com e
  sem (quase preta: sem GI no Baixo, média de 0,03 nits).
- **Custo por pixel**: com 64 luzes e 8 mapas, ~0,5 ms no Baixo. Somam a busca na tabela para cada luz com sombra da
  célula, os 4 ou 9 taps das que têm mapa e o degrau de registradores do `mesh_lsm`. Sem RT no Médio, com 16 mapas de 9
  taps, ~1 ms.
- **Testes**: `spec_leak_low` (Baixo, pico ≤ 30) e `spec_leak_nort` (`--no-rt` no Médio, pico ≤ 30), os dois com
  `--min-fps 200`. Mediram de 1 a 17 nits.
- **O que sobra**:
  - Faces de 64-128 texels, em luzes pequenas na tela, dão sombras mais moles.
  - Não há PCSS (penumbra pelo Source Radius).
  - Os tecidos não entram nos mapas das luzes locais.
  - Um objeto a menos de meio pixel na tela fica fora da lista de sombra e não entra no cache.

## ReSTIR DI nas muitas luzes (2026-11-09, plano de melhorias A9.1a) — Fotorrealista
Com o denoiser unificado ligado (`GI_RESTIR`, as variantes `RE_DEN` do `mesh.frag`), o sorteio das muitas luzes vira
um ReSTIR DI temporal (Bitterli et al. 2020): o sorteio do quadro (uma luz entre todas em proporção à luz sem sombra
dela: W = soma / a dela) junto com o reservatório deste ponto no quadro anterior (reprojetado pela profundidade, no
mesmo ator; a luz dele pesada pela luz que dá aqui; M até 7), um deles mantido em proporção a p · W · M; depois os
raios de uma luz só. O reservatório de uma luz escondida não é guardado (reaproveitamento de visibilidade): as luzes
visíveis passam de quadro em quadro. A parte que chega, p(y) · W · V(y) / a soma, vai para o split como ia a média das
duas visibilidades e o denoiser a filtra igual. O reservatório fica no `w` do guia (slot 16 bits | M 3 | W em 13 bits
de half); o `denoise.comp` o copia com o guia para o quadro seguinte. `REALENGINE_NO_RESTIR=1`: as duas luzes.
- **Medidas** (3060, Épico, 10 mil luzes com sombra, `many_lights` no Fotorrealista): 49-50 × 47 fps (uma luz traçada
  em vez de duas), erro contra o path tracing 32,9% × 34,2%, cintilação parecida (0,11-0,17% × 0,14%, varia por
  rodada). Ganho modesto: só o temporal.
- Teste `many_lights_restir`. **Falta:** o reaproveitamento espacial (vizinhos), ReSTIR GI (A9.1b), cache de radiância
  neural (A9.1c).

## Local Fog Volume (2026-11-09, plano de melhorias A9.5b)
Como o Local Fog Volume da Unreal (`COMP_FOG_VOLUME`, `CFogVolume`; Adicionar > Efeitos Visuais): neblina numa esfera ou
caixa (o espaço do ator, 100 cm por unidade de escala), densidade (1/m) no meio, suavizada nas bordas e para cima;
cor (albedo) e emissão próprias. `weather.c` põe até 16 num buffer do quadro (endereço em `GpuFrame.fog_volumes`, a
matriz mundo → espaço unitário de cada um) e o passe 3 do `weather.comp` soma a densidade deles à da neblina de altura em
cada froxel, com a luz de lá (sol com sombra por raio, céu, lâmpadas): os raios de sol e os halos atravessam o volume.
Sem neblina de altura os froxels rodam só por eles (densidade 0 na analítica). Precisa da neblina volumétrica (Médio
para cima) e só vale até a distância dela (60 m por padrão). `REALENGINE_NO_FOG_VOLUMES=1` compara. Cena `fogvol`, teste
`fog_volume`. Medida (3060, Épico): 300 × 333 fps, +0,33 ms com dois volumes (os froxels, que sem neblina não rodavam). **Falta:** no Baixo (analítica), além do alcance dos froxels.

## Câmera de cinema: lente, exposição manual, obturador e bokeh (2026-11-09, sobras da Fase 2, A9.4)
Como o Cine Camera Actor da Unreal, no `CCamera` (campos novos no fim: os níveis antigos abrem).
- `cine`: `CINE_LENS` (distância focal + largura do sensor -> FOV, aplicado no Details; abertura + Focus Distance ->
  profundidade de campo física), `CINE_EXPOSURE` (EV100 = log2(N²/t) - log2(ISO/100); a Exposure Compensation soma por
  cima, como na Unreal), `CINE_SHUTTER_BLUR` (Motion Blur Amount = tempo do obturador x Target FPS).
- `post_fill_view` (a câmera do jogador, só jogando): põe no `RenderView` `cam_manual_exposure`/`cam_ev100`,
  `dof_focal_mm`/`dof_sensor_mm`, `bokeh_blades`/`bokeh_rot_deg`; render.c: auto exposure desligada pela câmera; o
  `coc_of` do composite virou `min(|z - foco|/z x k, Max Bokeh Size)`: k = 8 x tamanho / f-stop (o modelo de antes,
  igual) ou `0,5 f²/(N (S - f)) / sensor x largura em px` (o círculo de confusão no sensor, em raio de pixels).
- Bokeh poligonal: `iris()` no composite escala cada amostra do disco até a borda do polígono no ângulo dela (lâminas e
  rotação em `GpuFrame.grade_lut.zw`).
- Details > Camera: Physical Lens, presets de filmback (Full Frame, Super 35, APS-C, M4/3, Super 16, IMAX 70) e de lente
  (12 a 200 mm com a abertura máxima), focal, sensor, abertura, foco, lâminas; Manual Exposure e Motion Blur from
  Shutter com presets (Sunny 16, Overcast, Indoor, Night Street, Cinema 180°), ISO, obturador como 1/x e o EV100.
- Teste `cine_camera` (`--cine-cam-test`: 50 mm no Super 35, Sunny 16, 6 lâminas, foco 6 m em Play): EV100 14,97 e FOV
  28,0°, sem erros de validação.
- Falta: HDR10 na saída (swapchain ST.2084), anamórfico, Blueprint para os campos. (Foco automático: o First Person
  Camera do Q12, `notas-dev/camera-primeira-pessoa.md`, usa esta lente e o bokeh.)

## Rastro nas sombras das luzes pontuais (2026-11-10, "ghosting na sombra" no Realista sem luz do céu)
- O histórico da visibilidade (`shadow_split.comp`) guardava 90% do quadro anterior preso só a ±0,25 do pixel atual:
  a sombra que saiu de um lugar virava 0,75 e sumia ~10% por quadro, várias cópias da Delight dançando no chão.
- Agora o histórico fica preso ao mínimo e máximo da visibilidade filtrada da vizinhança (as 3 linhas centrais da
  janela, como a caixa de um TAA) e, se a caixa o moveu mais de 0,1, o quadro novo pesa 0,5. Medido no MyProject
  (Realista, 2 luzes pontuais, Delight animando): uma silhueta só no chão; 155 fps (156 antes); `many_lights` com a
  mesma cintilação (1,20% contra 1,21% no vai-e-volta, 0,190% contra 0,189% no cache). Cena de teste `shadow_ghost`.


## Nuvens aparecendo ao virar a câmera (2026-11-10, relato do usuário)
- "As nuvens só aparecem quando olham... fica um quadrado da nuvem até aparecer a outra parte." Causa: nas nuvens da
  tela, uma direção nova (fora da vista do quadro anterior) só tinha valor nos pixels marchados naquele quadro (1 em 4);
  os outros 3 começavam como céu limpo e depois só ganhavam ~3% por marcha: um fade de ~1 s, e a borda da vista
  anterior virava um quadrado. Agora o pixel novo não marchado começa com o **panorama** (o céu todo, já com as nuvens)
  e as marchas só refinam (`weather.comp` passo 6). Custo: uma leitura de textura só nos pixels novos.
- Testes de luz e GI passam (20/20). Não verificado numa captura girando a câmera (não há modo automático para isso).

## Light Function (2026-11-17, plano 0.43 Q4c)
- **Domínio do material** (`Material.domain`, Details > Domínio do Material: Superfície / Função de Luz; JSON
  `"domain": "Light Function"`). O padrão é a **Emissão** assada (ou a Cor Base sem emissão); um nó **Time** anima (o
  loop em quadros, misturados como numa superfície).
- **Luzes** (`CLight.light_function`, `light_function_scale`, no fim do componente: níveis antigos leem 0): Details >
  Função de Luz em Spot, Point e Directional (lista os materiais do domínio, aceita arrastar do Content Browser);
  Directional: Escala da Função de Luz (cm, padrão 1000). Rect não (os meios-tamanhos são dela).
- **GPU:** `lights_gpu` põe o slot + 1 do material em `GpuLight.half_w` e a escala em `half_h` (só a luz retangular os
  lia). `mesh.frag light_fn` (variantes RE_EXT e RE_LSM; o render passa às RE_EXT quando alguma luz tem função:
  `lights_function_seen`) lê o material no mapa da luz: Spot pelo cone (o slide ocupa a base do cone), Point octaédrico,
  Directional no mundo projetado no plano da luz; multiplica `L.radiance` (colorido) antes de tudo (também no
  amostrador de muitas luzes).
- **Exemplos** (Materials/Examples/LightFunctions): `LF_Projector` (xadrez colorido), `LF_Blinds` (faixas de persiana),
  `LF_Clouds` (Wave distorcida andando pela fase do Time: nuvens), `LF_Flicker` (ruído branco por degrau de tempo).
- **Cena `lightfn`**: parede do fundo com o projetor (spot), parede lateral com a persiana (point), lâmpada piscando sobre
  uma caixa (spot), nuvens no sol (3000 lux: fim de tarde). `REALENGINE_NO_LIGHTFN=1`: as mesmas luzes sem função.
- **Medidas** (RTX 3060, 2560×1417, Épico, mínimo da GPU, 2 rodadas): sem 2,785/2,852 ms; com 3,084/3,139 ms: **+0,29 ms**
  com 4 luzes com função (inclui passar às variantes RE_EXT).
- **Neblina volumétrica e GI** (2026-11-17, "resolva as limitações"): a função foi para `shaders/lightfn.glsl`
  (`light_fn`, `light_fn_of`: 1 sem função), incluída no `mesh.frag`, no `weather.comp` (a dispersão da neblina por
  froxel: o sol com nuvens passando faz feixes, o projetor desenha o slide no ar) e, pelo `rt.glsl light_irradiance`,
  no `gi_cache.comp` (cache de superfície) e no `probe_trace.comp` (sondas): a luz rebatida também segue o padrão. Esses
  compute ligam o conjunto dos materiais (set 1, `mat_set()`) junto do set 0 (`weather.c`, `gi_cache.c`, `gi.c`).
  Medida (`lightfn`, Épico): 2,831/2,849 ms sem, 3,126/3,149 ms com (**+0,30 ms**; a GI 0,60 → 0,62 ms).
- Fora: o bake de luz (lightmaps estáticos) não grava a função (ela anima; como na Unreal).
- **Teste:** `light_function` (área `render`). Artigo `light-function`.

## Sombra distante do terreno e neblina na cor do céu (Q21, 2026-11-18)
Além do último nível das sombras virtuais o sol lê a sombra do terreno (um heightfield das Landscapes e o horizonte de
cada texel, `farview.c`), e dentro deles soma o que sobe além do alcance; a neblina analítica distante tira a luz ambiente
da cor do céu sobre o horizonte. Detalhes em [visao-distante.md](visao-distante.md).

## Feixes de sol nos mapas de sombra virtuais (2026-11-19, cena do metrô)
Num lugar fechado com frestas (o metrô abandonado, `demos.md`), os feixes da neblina volumétrica saíam em escada: um
raio de sombra no centro de cada froxel (128 × 72 × 64), aceso ou apagado inteiro, e os 2 raios do céu por froxel
deixavam uma mancha do tamanho de um froxel onde o teto tem buracos.
- **Sol pelos mapas**: com os mapas de sombra virtuais do sol ligados (`GI_VSM`, todos os presets), o passe 3 do
  `weather.comp` lê o mapa em 4 pontos espalhados dentro do froxel (ao longo da fatia e na área do pixel, num disco de
  Vogel girado por froxel), uma comparação bilinear no nível mais fino que contém o ponto (`vsm_fog_tap`). Vale a fração
  iluminada. Um ponto além dos níveis cai no raio de antes.
- **Ordem do quadro**: a neblina saiu do `weather_record` (que agora só faz as nuvens) para o `weather_record_fog`, depois
  dos mapas de sombra e das sombras das luzes locais, antes do passe principal. A barreira de antes de desenhar os mapas
  passou a esperar também as leituras do compute.
- **Tenda 3×3**: o passe 4 (integração) filtra a luz de cada fatia numa tenda 3×3, como luz por densidade (a borda de
  um Local Fog Volume não vaza), lendo os 10 × 10 froxels do grupo uma vez na memória compartilhada.
- Medida (RTX 3060, 1440p, Épico, cena `fogvol`, a build de antes × esta, outro editor usando a GPU): froxels 0,65-0,75 →
  0,44-0,50 ms; integração 0,02 → 0,05-0,08 ms; mínimo da GPU igual (3,06-3,08 × 3,15-3,18 ms). No metrô, os feixes
  saem lisos e separados; antes, blocos de froxel.
- **Falta**: a resolução dos froxels (um feixe mais fino que ~20 px de tela some no froxel); os reflexos por raio não
  somam a neblina.

## Material emissivo ilumina (2026-11-23)
"Quero que objetos que tenham materiais emissivos iluminem e também tenham luz indireta." Antes o brilho de um material
era só somado na tela: `rt.glsl` zerava a emissão de todo material nos raios, nada iluminava.
- **Luz direta**: o ator cujo material brilha o bastante vira uma **luz pontual** no próprio slot (`lights.c
  lights_emissive_light`; um mesh não tem CLight ali): no centro dos limites, esfera do raio deles, candelas = emissão
  média (nits) × área da malha (m²) / 4 × Emissive Light do material; alcance até 0,5 lux (máx. 40 m). Entra na grade,
  na amostragem estocástica, no ReSTIR e na luz direta da GI sem mudar os shaders de iluminação. **Sem sombra por
  padrão**; **Emissive Shadows** (no material) liga a sombra **só por raio**: `half_h = −raio` marca a luz e `light_skip`
  faz os raios de sombra pararem na esfera (senão batiam na própria malha); o mapa de sombra das luzes locais pula essas
  luzes. Sem RT: sem sombra. `REALENGINE_NO_EMISSIVE_LIGHTS=1` desliga todas (o "antes" das medidas). Sempre no conjunto comparado: editar o
  material, mover ou esconder o ator aparece no quadro seguinte. Abaixo de 0,5 cd (um LED) só a GI.
- **Emissão média** do material: o último mip da textura de emissão lido junto com a cor média no bake
  (`Variant.avg_emis`, `Material.avg_emis`).
- Medido (RTX 3060, 1440p): `--scene emit` (sala fechada sem luz, esfera de 40 cm a 2000 nits, com Emissive Shadows):
  493 fps (GPU 1,96 ms) contra 635 fps com Emissive Light 0 (1,47 ms). `--scene materials --render-style 2` (dois
  painéis emissivos): 216 fps sem as luzes, 204 sem sombra, 179 com sombra — a primeira luz local com sombra liga a
  amostragem das muitas luzes e o filtro de sombras (~1 ms): por isso a sombra é opcional (o `render_style_photoreal` e
  o `budget_house` voltaram a passar). Testes `emissive_light` (o chão iluminado na captura, 1 luz local, com sombra) e
  `emissive_light_off` (nenhuma luz com Emissive Light 0).
- Fora: luz de área exata pela forma (é uma esfera equivalente: um painel longo ilumina como uma esfera do tamanho dele);
  emissão animada/por textura por ator (usa a média do material).
