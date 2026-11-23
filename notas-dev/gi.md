# Iluminação global híbrida ("RealGI")

Pedido: estático com GI "baked" pelo próprio sistema (e bake completo possível), dinâmico com GI em tempo real,
realista, **sem o ghosting da Lumen**, e dentro do lema (> 200 fps, ideal > 350 a 1440p na RTX 3060: orçamento de
GI ≈ 1,0–1,5 ms de GPU no preset High). Proposta aprovada e implementada (passos 1–6 abaixo); a proposta original
segue depois da seção "Implementação".

## Implementação
Código: `gi.c`, `gi_bake.c`, `render.c`; shaders `gi.glsl`, `rt.glsl`, `probe_*.comp`, `lightmap_bake.comp`,
`pathtrace.comp`, `exposure.comp`, `mesh.frag`.
1. **Ray tracing**: um BLAS por malha (na inicialização), TLAS de todas as instâncias refeito por frame (máscara:
   0x01 visível, 0x02 projeta sombra, 0x04 Static). Ray query inline em compute e fragment. **Path Tracing**
   (modo de visualização, Shift+P: o P é o NavMesh): referência que acumula enquanto câmera/luzes/atores não mudam.
2. **Sondas dinâmicas**: grade 32x32x12 a 200 cm em volta da câmera, toroidal (slot = célula do mundo mod grade),
   sondas no centro das células, 64 raios (Medium: 32), 1/4 das sondas por frame. Irradiância octaédrica 8x8 (E/π,
   RGBA16F) + momentos da distância (RG16F), borda de 1 texel, Chebyshev contra vazamento. Histerese **por sonda**
   0,97, cai para 0,75 onde a luz mudou muito, zera quando o slot passa a outra célula. A história mora no mundo:
   girar/andar não tem ghosting. **Reflexos por raio** em superfícies lisas (pipeline `mesh_rt`, só com eles ligados).
3. **Build Lighting** (menu Build: Preview 256 amostras, Production 2048): UV2 por forma (atlas, ~10 cm/texel),
   rasterização do UV2 na CPU em G-buffer (posição, normal), bake progressivo na GPU visível no viewport (raios
   cosseno só contra os Static, luzes com raios de sombra, bounces das sondas, céu nas falhas), resolve + dilatação.
   L0 (E/π) e L1 (direção ponderada). `<nível>.rlight` por entidade com hash do transform: mexeu num Static →
   volta às sondas e aparece "LIGHTING NEEDS TO BE REBUILT".
4. **Volume de sondas baked** (`.rlight` v2, v1 ainda abre): no bake a grade se ajusta aos limites dos Static
   (espaçamento ≥ 50 cm, uma célula de folga), todas as sondas por frame, raios só contra os Static, média corrida;
   48 frames de aquecimento antes dos lightmaps lerem os bounces. **Mistura por mobilidade**: Static com lightmap →
   lightmap; o resto → sondas dinâmicas quando ligadas, senão o volume baked (amostrado como está, **sem RT**), senão
   o SH do céu. Lightmaps e volume abrem e funcionam sem ray tracing (`--no-rt` testa).
5. **AO de contato** (Settings > Contact AO): desvio da proposta. Sem pré-passo de profundidade/normal (forward MSAA),
   AO de tela ou raios curtos filtrados exigiriam refazer os passes; em vez disso os 32 atores **Movable** mais
   próximos da câmera viram oclusores analíticos (caixa orientada; esfera, cilindro e cone como esfera) e cada pixel
   calcula a oclusão ponderada pelo cosseno exata (fórmula de Lambert por face). Sem raios, sem ruído, sem história,
   roda sem RT; cobre o que o bake não vê (um ator sobre o chão baked, atores encostados). Escurece a indireta e o
   reflexo do ambiente (não o reflexo traçado, que já vê o oclusor).
6. **Exposição automática** (viewport > Auto Exposure, ligada por padrão): histograma de 64 bins do log da luminância
   de 1 pixel em 4x4, média entre os percentis 10 e 90, convertida em EV100 pela câmera física ([luz.md](luz.md)),
   adaptação suave (3/s para o claro, 1,5/s para o escuro), tudo na GPU; o slider de exposição vira compensação.
   **Presets**: Low = só baked (lightmaps + volume) + AO de contato; Medium = + sondas com 32 raios; High = 64 raios
   + reflexos (rugosidade < 0,3); Epic = reflexos até 0,5.

### Medidas (RTX 3060, 2560x1417, viewport ~2100x1350, MSAA 4x, nível padrão, `--bench 3`)
| Configuração | fps | GPU ms | detalhe |
|---|---|---|---|
| Sem RT (`--no-rt`) | 1272 | 0,69 | só SH do céu + AO |
| RT, sondas desligadas | 1207 | 0,74 | TLAS 0,04 |
| Sondas (`--gi probes`) | 573 | 1,67 | sondas 0,22; principal 1,15 (coleta das 8 sondas ~0,6) |
| Completo (`--gi full`, High) | 569 | 1,68 | + reflexos |
| Completo + lightmaps/volume | 872 | 1,05 | o chão lê o lightmap em vez das sondas |
| Só baked (RT, sondas desligadas) | 1138 | 0,79 | |
| AO de contato (3 oclusores) | — | 0,08 | |
| Exposição automática | — | 0,027 | |
| Build Lighting Production (2 Static, atlas 1024) | — | — | 0,66 s com o aquecimento |

Testes (`ctest`): `editor_fps` (padrão = High, > 350 fps), `gi_bake` (bake Preview + só o caminho baked, com
validação: falha em qualquer erro do Vulkan), `gi_no_rt` (sem ray tracing, nível padrão sem bake), > 350 fps cada.

### Cache de superfície (tempo real, `gi_cache.c`, `gi_cache.comp`) — o GI dinâmico principal
Pedido: uma casa fechada no escuro com uma spot na parede: a indireta não chegava ao teto e ao chão, vazava pelas frestas
e piscava. Causa: as sondas de 2 m são maiores que as paredes (ficam dentro delas, interpolam através delas) e o ruído
de 64 raios disparava a resposta rápida (histerese 0,75) o tempo todo.
- **Radiosidade nas superfícies**: cada malha a até 60 m da câmera ganha um quadro num atlas de 1024² (UV2 da forma;
  ~15 cm por texel no High, 10 no Epic, 30 no Medium, mais grosso com a distância), cada texel um ponto da superfície
  em espaço da malha (segue um Movable). Por frame uma parte dos texels (64 k no High) recebe: direta exata (raio de
  sombra) e 8 raios cosseno cuja batida lê o próprio cache (direta + indireta da superfície atingida: várias
  reflexões, sem raios de sombra na batida). Os raios saem da superfície: só atravessam frestas de verdade.
- **Sem vazamento**: texel cujo meio dos raios vê costas está dentro de outra malha (validade 0, pré-multiplicada: a
  leitura bilinear o ignora); texel de borda vai meio texel para dentro (linha clara onde a parede encosta no chão);
  quadro medido pela maior face (o UV2 do cubo dá a mesma área a todas); texel menor que uma parede de 20 cm.
- **Sem piscar**: histerese 0,98 por atualização, 0,5 só quando a mudança passa de 4× o ruído medido do texel;
  leitura com 4 amostras (tenda 3x3).
- Pixel: lightmap (Static com bake) > cache > sondas (só para malhas sem quadro). Reflexos e as próprias sondas leem
  o cache na batida. Realocação quando a cena muda perto da câmera ou ela anda 15 m (a luz dos quadros mantidos é
  copiada: sem clarão).
- **Ressalto dos Movable** (`occluder_light.comp`): os 32 oclusores do AO de contato têm 6 radiâncias (faces da caixa
  ou cubo de ambiente da esfera, com raios de sombra) e o AO vira GI de contato: fator de forma × radiância. A bola
  vermelha no sol tinge o chão ao lado, com ou sem lightmap.
- **Sondas**: histerese por sonda com o ruído medido (média e desvio da luminância dos raios); sonda com 25% de raios
  em costas (dentro de objeto) fica de fora da amostragem.
- **Objetos longe com manchas escuras** (pontos/listras): o raio de sombra saía de um LOD grosso, que fica dentro do
  LOD 0 da BLAS. Agora o raio sai a `lod_error` (sagita da corda do LOD × escala, `mesh.vert`) da superfície.

- (2026-11-19, sobra do Q8) Os texels dos Movable **em movimento** (passe 2) e o filtro deles (passe 3) vão numa **lista
  de índices** (buffer por quadro em voo, até 96K) e um dispatch cada; antes era um dispatch por sequência de cartas
  vizinhas (milhares com 2500 caixotes caindo) e, passando de 128 sequências, o filtro rodava no atlas inteiro (1 M
  texels) todo quadro. Cache de superfície no armazém de 20 mil: 1,69 → 1,40 ms.

### Estável e instantâneo (2026-10-30, segunda rodada)
- **Quadros retangulares** pela maior face (Jacobiano do UV2): parede 10 x 3 m = 256 x 64 texels quadrados (antes
  quadrado com texels 27 x 5 cm: o ruído virava listras claras que pareciam luz passando por frestas).
- **Média corrida** por texel (peso 1, 1/2, 1/3... até 512 atualizações; `meta`: ruído, contagem, desvios seguidos):
  a primeira atualização já acende o texel, as próximas só refinam; mudança real = 3 desvios seguidos no mesmo
  sentido acima de 3σ → a média encurta para 1/4 (repetindo, converge rápido); alarme falso quase não mexe. Sem
  corte de outliers (num sistema de vários ricochetes ele travava texels escuros).
- **Rajada**: nos 4 frames depois de uma alocação todos os texels atualizam (a luz aparece de uma vez).
- **Exposição automática** com zona morta de 0,1 EV (as oscilações pequenas do histograma moviam a imagem toda).
- Flicker (mudança média; e a parte da vista que muda > 2% por frame, `--bench`): casa 3,96% (só sondas) →
  **0,029% e 0,00%**; nível padrão 0,002%; 40 k atores 0,012% e 0,2% (horizonte: as sondas, fora do alcance do
  cache). `REALENGINE_FLICKER_MAP=arq.pgm` grava onde pisca. Teste `gi_house`: flicker < 0,1%.
- **Frestas (luz atravessando paredes)**: uma parede comprida que passa pelos dois cômodos tem um quadro só; a leitura
  bilinear (alcance ~18 cm) pegava, ao lado da parede interna (20 cm), os texels do cômodo aceso. Agora a leitura só é
  bilinear quando o texel sob o ponto é válido; enterrado (embaixo de uma parede), o vizinho válido mais escuro (a luz do
  outro lado é sempre a mais clara). Raios da GI leem do mesmo jeito (direta e indireta pré-multiplicadas pela
  validade), batida a < 10 cm usa só a direta (dois cantos se realimentavam). Superfície com quadro nunca usa sondas.
- **Paredes por texel (o que ainda vazava)**: cada texel guarda o passo de um texel ao longo do x e do y do atlas
  (Jacobiano da UV2, `CacheTexelGpu` agora 64 B) e mede com 4 raios curtos (1 cm acima da superfície, 1,5 texel) a
  distância até uma parede em ±x/±y; a 4ª imagem do cache (bindings 36/37) guarda `1 − d/1,5`. A leitura (`cache_read`)
  é bilinear manual: só entra o texel sem parede entre o centro dele e o ponto; nenhum dos 4: o válido mais escuro dos 8
  vizinhos que pode, senão o válido mais escuro. Duas causas achadas com diagnósticos no shader: (1) o vizinho válido mais
  escuro procurava só em 4 direções, e no rodapé (fileira de borda) o único válido era o do lado aceso (ponto branco no
  canto); (2) o ponto dos texels de borda era empurrado meio texel **para o centro do triângulo**, de lado ao longo da
  parede, atravessando parede fina; agora vai reto para dentro a partir da aresta. Paredes medidas nas 2 primeiras
  atualizações do texel, depois 1 em 8. Custo: GI 0,30 → 0,38 ms na casa (a leitura busca mais texels), 445–453 fps.
- Cena `--scene house_thin` (parede interna de 5 cm, menor que o texel de 15 cm) e teste `gi_house_thin`. Parede de 20 cm:
  limpa. Parede de 5 cm: o bloco de um texel virou um ponto de ~2 cm no pé do canto de três peças (um texel do lado escuro
  bem no canto guarda luz clara; a causa não foi achada: não é a costura a < 2 cm nem a leitura).
- **Flicker no jogo**: o Character parado afundava um fio por frame e voltava (a gravidade agia sem parar e a colisão só
  pega acima de 0,01 cm), mexendo a câmera presa nele: o chão inteiro piscava (8,9% em 22% da tela). Agora sem gravidade
  no chão, sonda de 1 mm e `ecs_touch` só quando o transform muda: 0,036%. Teste `play_flicker` (`--bp-idle`).
- **Ator em movimento** (`INST_MOVING`: um Movable cujo transform mudou nos últimos 60 frames, `render.c scene_prepare`):
  andando ou caindo, o chão em volta dele piscava em blocos do tamanho de um texel (os raios do cache o viam: texels sob
  ele inválidos, ele contado como parede, sua sombra com atraso de algumas atualizações), e as sondas guardavam a sombra
  dele e devolviam um rastro escuro nos pés. Agora a TLAS tem as máscaras "parado" (0x08 visível, 0x10 sombra: toda
  instância estática e todo Movable parado); o cache e as sondas (`RT_STILL_ONLY` em `rt.glsl rt_mask`) só veem o que está
  parado. O ator em movimento é sombreado pela GI de contato, por pixel e sem atraso. Os texels dele são refeitos a cada
  frame (passada 2 do `gi_cache.comp`, só os quadros em movimento, escolhidos na CPU: 600–1400 texels), com histórico de 8
  e leitura de 5 amostras (`cache_indirect_soft`: as manchas que andavam com ele). Parado, um Movable é parede como
  qualquer outra: importa porque **todo ator novo nasce Movable** (`ecs.c`), e as paredes de uma casa feita no editor são
  Movable — cena e teste `gi_house_movable`.
- **Costura das malhas redondas** (a linha vertical no corpo do Character): o lado de um cilindro/cone/esfera é um quadro
  só da UV2 que fecha em si mesmo (x de 0,02 a 0,98); as duas bordas são o mesmo lugar, mas iluminadas por texels
  diferentes, e a leitura de 5 amostras do ator em movimento não atravessava a borda: um degrau de ~+3 níveis (8 bits) bem
  no meio das costas, na visão GI Only. Agora `MeshInfo.uv2_wrap` (render.c: esfera, cilindro e cone) diz onde o quadro
  fecha; `cache_read_uv2` (gi.glsl), a menos de 1,5 texel da costura, mistura a leitura com a do mesmo ponto do outro lado
  (meio a meio na costura) e as amostras vizinhas passam para o outro lado. Medido: o pico some (perfil liso), custo nulo
  fora da faixa da costura.
- **GI de contato numa superfície com cache**: só os atores em movimento (os parados o cache já vê). Uma casa de paredes
  Movable fazia cada pixel testar as 9 caixas enormes (cantos escurecidos em dobro): 253 → 409 fps.
- Teste `house_walk` (`--scene house --bp-walkview`: o personagem anda pela casa, câmera parada): 0,035%, 414 fps.
  Personagem parado 417 fps; casa 431 fps; jogo empacotado com Blueprints 396 fps (a primeira execução de um executável
  recém-empacotado é mais lenta: o driver compila os shaders; o teste mede 3 s).
- Sondas com a mesma média corrida (até 64) e as distâncias com essa história; ponto fora da grade lê a borda mais
  perto sem o teste de visibilidade (os 8 pesos esmagados faziam o horizonte piscar): 40 k atores 0,20% → 0,02%.

Medidas (RTX 3060, 2560×1417, High, `--bench 3`; flicker = mudança média da luminância entre frames, câmera parada):
| Cena | Só sondas | Cache de superfície |
|---|---|---|
| Casa (`--scene house`) | 306 fps, flicker 3,96% | **435 fps, flicker 0,15%** |
| Nível padrão | 454 fps | **572 fps** (os pixels com cache não coletam 8 sondas) |
GI inteira 0,29 ms na casa (164 k texels, 64 k por frame). Teste `gi_house` (validação, > 350 fps, flicker < 0,5%).

### Falta / próximos
- Unidades físicas, câmera EV100, céu atmosférico e sombras por raio: feitos, em [luz.md](luz.md).
- Coleta das sondas a meia resolução (o principal custo do High, ~0,6 ms); BC6H/zstd no `.rlight` (31 MB hoje,
  16 MB de lightmaps + 15 MB de volume); denoise do lightmap Preview; oclusores por tile quando houver centenas de
  atores Movable; bake sem RT na CPU (BVH do Tlender); reflexos a meia resolução com denoise curto.

---

## Proposta original
### Ideia central
Um **único traçador de raios na GPU** (VK_KHR_ray_query sobre BLAS/TLAS das malhas) alimenta as duas metades:
- **Tempo real**: poucos raios por frame, acumulados **no espaço do mundo** (sondas), nunca no espaço da tela.
- **Bake**: o mesmo traçador rodando até convergir (milhares de amostras), gravando o resultado. "Build Lighting"
  é o RealGI deixado rodando até o fim — a mesma luz, sem diferença visual entre baked e dinâmico.

### Camadas
1. **Lightmaps (Static)**: cada malha Static ganha UV2 (desdobrado automático para as formas básicas; o `.rasset`
   traz UV2 do Tlender). Por texel: irradiância **direcional** (SH L1, 4 coef. RGB → BC6H), que mantém o normal map
   vivo. Gravados ao lado do nível (`Mapa.rlight`). Custo em tempo real: uma leitura de textura.
2. **Volume de sondas baked**: grade esparsa (densa perto da geometria) com irradiância octaédrica + visibilidade
   (média e variância da distância, teste de Chebyshev → **sem vazamento de luz** pelas paredes). É o que os objetos
   Movable usam para receber a luz baked do cenário.
3. **Sondas dinâmicas (Movable / luzes que mudam)**: clipmap de sondas em cascata ao redor da câmera (estilo DDGI).
   A cada frame uma fração das sondas lança N raios (ray query) contra a cena **incluindo os objetos dinâmicos**
   (BLAS reajustado por frame). Histerese **por sonda**: rápida onde a luz mudou (detecção de mudança + invalidação
   quando um objeto entra no volume da sonda), lenta onde está estável. A história mora na sonda, que não se move
   com a câmera: **girar ou andar não gera ghosting**.
4. **Detalhe de curto alcance**: AO/GI de contato por pixel (raios curtos ou tela) **sem histórico temporal**, só
   filtro espacial guiado por profundidade/normal. Sem história = sem fantasma.
5. **Reflexos**: rugosidade baixa → raios refletidos (meia resolução no High, cheia no Epic) sombreados com
   lightmap/sondas no ponto atingido; rugosidade alta → sondas. Denoise espacial + temporal **curto e estrito**
   (vetores de movimento, rejeição por profundidade/normal/ID do objeto, no máximo 4 frames, clamp de variância).

### Mobilidade (como a Unreal)
| Mobilidade | Indireta | Direta |
|---|---|---|
| Static (objeto) | lightmap baked | sombra baked (luz Static) ou em tempo real (Stationary/Movable) |
| Movable (objeto) | sondas (baked + dinâmicas) | tempo real |
| Luz Static | entra só no bake | baked |
| Luz Stationary | indireta baked | direta e sombra em tempo real |
| Luz Movable | sondas dinâmicas | tempo real |

### Presets (Settings > Engine Scalability)
- **Low**: só baked (lightmaps + sondas baked) + AO de contato. Roda sem RT por hardware.
- **Medium**: + sondas dinâmicas com poucos raios.
- **High** (padrão): + reflexos em meia resolução. Alvo ≈ 1,2 ms.
- **Epic**: mais raios, reflexos em resolução cheia, AO por raio.
- Sem ray tracing por hardware: bake na CPU (copiar e adaptar a BVH SAH e os kernels do Tlender) e só os presets
  Low/Medium.

### Ordem de implementação (cada passo medido e com teste de regressão)
1. BLAS/TLAS das malhas e ray query; modo de visualização **Path Tracing** (referência de "verdade" para comparar).
2. Sondas dinâmicas + visibilidade + histerese por sonda (o GI em tempo real).
3. UV2 automático, bake de lightmaps progressivo (preview no viewport enquanto converge), `.rlight`.
4. Volume de sondas baked e a regra de mistura por mobilidade.
5. Reflexos por raio e AO de contato.
6. Presets, exposição automática, unidades físicas (lux/candela), medidas finais.

### Por que não é a Lumen
A Lumen acumula no espaço da tela (screen probes/traces + reprojeção temporal longa) e atualiza o cache de
superfície devagar: ao mover a câmera ou um objeto a história reprojetada fica errada → ghosting e "lag" de luz.
Aqui a única história longa vive no mundo (sondas, lightmaps); o que é por pixel não tem história ou tem uma muito
curta com rejeição estrita.

### Casa do usuário, marca do player, indireta lenta (2026-10-31)
- **Lightmaps vazando nas quinas e "pintados à mão"** (nível de teste do usuário: paredes de 4–5 cm de cubos que se
  cruzam): o vazamento era do **bake** (o mesmo com a GI dinâmica desligada). Os quadros eram quadrados pela área total
  (numa parede de 5,8 m o texel tinha ~27 cm no comprimento), lidos por bilinear simples (a luz do lado de fora da parede
  que cruza entrava), e os ricochetes vinham das sondas de 2 m (manchas grandes). Agora o bake usa **os texels do cache**
  (`gi_cache_chart`: quadros retangulares pela maior face, pontos empurrados para dentro a partir das arestas, passo do
  texel no mundo), e `lightmap_bake.comp` tem a passada 2 (uma vez): **validade** (16 raios; ¼ em costas = texel dentro de
  outra malha) e **paredes por texel** (4 raios ao longo do atlas, 3ª imagem do lightmap, bindings 38/39). A leitura do
  lightmap é a do cache (`atlas_read`: só texels válidos e sem parede entre eles e o ponto). **Ricochetes do próprio
  lightmap** (um raio que bate num Static lê o lightmap ali: várias reflexões no detalhe do texel; 12 passadas de
  aquecimento e a média recomeça) e **denoise 5x5** na resolução (mesmo plano/normal, sem parede, só válidos). Raios em
  costas não contam como escuro. `.rlight` v4 (+ paredes); v3 abre sem paredes (bilinear como antes): **refazer o Build
  Lighting**. Medido no nível do usuário: Production 1,4 s, 494 fps; quinas sem brilho, paredes lisas.
- **Marca do player no chão** depois que ele sai: parado ele vira parede para os raios do cache; os texels em volta têm
  média de até 512 atualizações e tirar a sombra dele muda pouco cada texel (abaixo do teste de 3 desvios seguidos): a
  sombra velha ficava segundos. Agora `render.c` gera um **evento** (centro, raio) quando um Movable começa/para de andar
  (ou aparece/some); por 2 ciclos do cache os texels dentro dele têm história curta (`GpuFrame.cache_events`).
- **Indireta lenta ao entrar numa sala clara**: a média corrida desde o início misturava as primeiras atualizações (com
  menos ricochetes, mais escuras) e clareava em segundos; uma deriva lenta nunca disparava o teste. Agora uma **média
  rápida** (¼ por atualização, amostra limitada a 3 ruídos da média longa) ao lado da longa: passou de 2 ruídos, a
  história cai pela metade (mínimo 8) a cada atualização enquanto durar. Quadro que muda de resolução (a câmera se
  aproxima) tem a luz **reamostrada** (blit linear, pré-multiplicada) em vez de recomeçar escuro; conta como 4 atualizações.
- Flicker (`--bench 3`): casa 0,034% (antes ~0,03–0,05), casa Movable 0,039% (antes 0,05%); 19/19 testes.

### Túnel do FirstPersonMap do usuário (2026-10-31)
- O túnel (paredes de 40 cm, teto de 11 cm) é todo **lightmap** (modo GI Source: verde). As manchas de "denoise ruim"
  e as linhas azuladas nas junções vinham do `.rlight` salvo antes das correções de vazamento / com poucas amostras:
  refeito com o código atual, as linhas somem.
- O Preview tinha **256 amostras** por texel: com a luz entrando só pelas bocas do túnel isso lia como fumaça. Agora
  Preview **1024** (8 raios por passada) e Production **8192** (16): no nível do usuário 4,7 s e 8,3 s (antes ~1,4 s
  o Production de 2048). O denoise do bake ficou adaptativo: raio 4/3/2 e variância 5/3/1,8 texels² conforme as
  amostras já somadas, com um peso fraco por luminância (bordas de sombra) além de plano/normal/paredes/validade.
- Para o usuário: **refazer o Build Lighting** no projeto (o arquivo antigo continua abrindo).
- `--list-actors` (linha de comando): lista nome, posição, rotação e escala dos atores do nível no log.

## Personagens de poucos polígonos (2026-11-01)
- **Sombra por raio sem blocos**: as variantes RE_RT do `mesh.frag` leem os 3 vértices do triângulo
  (`VK_KHR_fragment_shader_barycentric`, exigida junto com o ray query) e tiram a origem do raio de sombra da
  superfície curva das normais (Hanika, "Hacking the Shadow Terminator", RT Gems II). Antes cada triângulo plano
  sombreava o vizinho: a pele do Delight virava um mosaico de quadrados.
- **Cache de superfície**: malha importada com menos de 6 texels por triângulo no chart (personagem: centenas de
  triângulos em poucos texels) fica sem chart — cada triângulo tinha luz própria; as sondas a iluminam suave.

## Relatórios da 0.14: piscar nas demos, landscape preto, indireta só de perto (2026-11-03)
Bugs #27/#30 (Terror e RPG piscando), #42 (landscape sumindo/preto/pixelado ao andar), #55 (indireta só perto do objeto).
Uma causa comum: **o atlas do cache (1024²) não cabia a cena no Épico** e a alocação descartava as malhas mais longe, que
caíam nas sondas (2 m, vazam, piscam nas paredes; enterradas num morro dão preto). RPG Épico: 85 de 209 malhas com quadro
(tudo além de ~16 m nas sondas: as casas que piscavam); Terror: 6 de 67 (lajes de cubo de 12 x 9 m: o UV2 do cubo dá a
mesma área às 6 faces, cada laje pegava 512 x 256 texels).
- **Alocação adaptativa** (`gi_cache.c allocate`): enquanto não cabe, os texels engrossam mais cedo com a distância
  (`falloff` 15 m → 4,5 m, × 0,7 por passo) e depois em toda parte (`coarse` × 1,25, até 6×); só então o mais longe sai
  (pulando o que não cabe, não parando nele). Não desiste "quando não ajuda": os lados são potências de 2 e um passo muitas
  vezes não muda nenhum. O `--bench` mostra `quadros/candidatos`, `x coarse` e `coarser past N m`; `--cache-all` falha se
  alguma malha ao alcance ficou sem quadro.
- **Malha densa** (regra dos 6 texels por triângulo): decidida na densidade de perto, nunca pela distância. Antes uma malha
  importada tinha quadro de perto e, longe, o quadro mais grosso caía abaixo de 6/triângulo: sondas — a indireta "só
  aparecia de perto" (#55). Agora, longe, o quadro cresce de volta até 6/triângulo (no máximo o de perto).
- **Blocos do Landscape** (`CAttach` `ATTACH_LANDSCAPE`, nó < `LANDSCAPE_PLANT_NODE`): fora da regra densa (chão suave,
  sem mosaico) e com o quadro pelo tamanho em x/y (o UV2 é o bloco visto de cima; o fator 1,6 das importadas dobrava os
  texels). Antes todos iam para as sondas (#42: preto nos morros, onde as sondas ficam enterradas).
- **Distância pela caixa da malha** (centro girado + meia diagonal × escala), não pelo raio do cubo unitário (87 cm): um
  bloco de 50 m sob a câmera contava dezenas de metros mais longe.
- Medido (RTX 3060, 1440p, Épico, editor parado; GPU dividida com outras sessões, A/B alternado):

| Cena | Antes | Depois |
|---|---|---|
| RPG `--cam -300,-600,250,60,-8` | 85/209 quadros, flicker 0,46%, 3% da vista; GI 1,68 ms, ~190 fps | 209/209, **0,12%**, 0,06% da vista; GI 0,82 ms, ~205 fps |
| Terror jogando | 6/67, flicker 0,131%, 4,5% da vista | 67/67, **0,022%**, 0,02% da vista; ~150 fps (antes 98–134) |
| Landscape (`--scene landscape`) | blocos todos nas sondas | 806/806 quadros (19,5 cm perto) |

  O resto do flicker do RPG eram as **nuvens na tela** ([luz.md](luz.md)). Testes `demo_rpg_flicker` (< 0,25%) e
  `demo_horror_flicker` (< 0,06%), ambos `--cache-all`; `landscape` com `--cache-all`.
- As sondas ainda piscam onde são usadas (além de 60 m, malhas densas): ~0,15% numa parede só com elas. Próximo: quadros por
  face para o cubo (as lajes finas desperdiçam ~4× do atlas) e o ruído das sondas.

## Bissecção da queda de ~5% depois dos merges da 0.15 (2026-11-03)
**Corrigido em 2026-11-04** ([render.md](render.md), "Registradores do passe principal"): a bissecção abaixo comparava
builds depois de ea1730d entre si, com a média de fps, e não via a queda. Contra a 0.14, com a GPU mínima, ela existia
(7–10% no Alto/Épico): era o critério do #32 junto com o rt.glsl, empurrando o shader para o spill de registradores.

A sessão principal mediu na main com os merges (f0a2211, b219be3) `house_walk` ~291 → ~275 fps e `play_flicker` em
0,102–0,106%. Bissecção com quatro builds lado a lado, todas com o ajuste do rt.glsl (anéis só no Épico, ca5d6ed):
- A: ea1730d, antes dos merges;
- B: 366741f, o cache para toda a cena, as nuvens e o reflexo do chão;
- C: f5a27a3, prévias e Landscape/Foliage;
- D: a main;
- E: a main com o critério antigo do reflexo (uma chave temporária, já tirada).

Medida: A/B alternado, mediana (máx), RTX 3060 1440p.

| Cena | A | B | C | D | E |
|---|---|---|---|---|---|
| `house_walk` (Épico, GPU livre, 4 rodadas) | 258 (263) | 261 (266) | 258 (265) | 255 (258) | 254 (258) |
| `play_flicker` (4 rodadas) | 226, 0,098% | 225, 0,099% | 223, 0,099% | 217, 0,100% | 203, 0,101% |
| RPG Médio (4 rodadas) | 248 (313) | 239 (329) | — | 200 (247) | — |
| RPG Alto (4 rodadas) | 176, **0,46%** | 184, 0,08% | — | 181, 0,10% | 180 (5 rodadas, D 180) |
| RPG Épico (9 rodadas) | — | — | — | 160 (213) | 159 (217) |

- **Nenhum dos dois merges custa fps mensurável.** O menor tempo do passe main fica igual em todas: casa 1,89–1,94 ms,
  idle 2,20–2,22 ms. A diferença entre as rodadas, até de 290 para 255 na mesma build, vem da GPU: outras sessões rodavam
  o Tlender, e a GPU esquentava. Ela é maior que qualquer diferença entre as versões.
- **A casa é fechada, sem sol nem céu.** O critério do #32 não dispara ali (`sky_spec` = 0), e o cache já cabia
  inteiro (38/38 quadros, 447520 texels, os mesmos de antes). No RPG, o critério novo custa o mesmo que o antigo no Alto
  e no Épico. No Médio não há reflexo por raio (`render_scalability`), então o #32 não pesa nele. O cache para toda
  malha também não pesa: são 16384 texels por quadro em qualquer cena. Por isso nada foi posto atrás de preset.
- A build da main tem 13 MB na `Release/Content`, deixados pelos testes: 37 malhas importadas, 511 draws indiretos na casa
  contra 67. A build A com essa mesma Content (F) dá 205 contra 209 fps (máx 275 contra 271): ≤ 2%, dentro do ruído.
- `play_flicker` > 0,1% e os limites de fps já falham em ea1730d nesta GPU (~226 fps contra os 273–319 de render.md).

## GI sem manchas enquanto carrega (2026-11-04)
O cache de superfície mostrava manchas escuras, tipo "perlin noise", nos primeiros segundos (e onde uma luz mudava):
cada texel faz a média de todas as atualizações desde o começo, e as primeiras chegam escuras (os rebotes ainda não
chegaram) e com poucos raios. Três mudanças em `gi_cache.comp`/`gi_cache.c`:
- **Filtro no atlas (passe 3)**: 5x5 com bordas (luminância a 4σ do ruído do próprio texel), dentro do quadro (chart)
  do texel, nunca atravessa parede (`atlas_lends`), peso pela confiança de cada vizinho (contagem de atualizações). Um
  texel novo pega a luz dos vizinhos assentados em vez de aparecer como borrão escuro. Só os texels atualizados no
  quadro (a cota do passe 1 e as corridas dos atores Movable do passe 2), logo depois da atualização: o que os pixels
  leem só muda quando um texel muda. A imagem nova (binding 50) é a que o passe principal e os rebotes amostram
  (binding 23). `REALENGINE_GI_FILTER=0` desliga para comparar.
- **Aquecimento**: nas 8 primeiras atualizações (fora dos atores Movable) a histerese fica em 0,5: segue a luz chegando
  em vez de guardar as primeiras amostras escuras; 2x raios enquanto a contagem é < 4; no primeiro segundo de uma
  alocação, 3x a cota por quadro.
- Texel sem luz ainda continua sem luz (os pixels caem para as sondas), o filtro não o inventa.

Medidas (RTX 3060, 1440p, Alto, GPU mínima): GI da casa 0,401 → 0,442 ms no primeiro segundo (cota 3x), igual depois;
`--bp-idle` 0,320 → 0,335 ms (+0,015 ms). Cintilação (`play_flicker`, mediana de 5): 0,17% → 0,08%. Na casa, 0,6 s
depois de abrir: sem filtro o chão tem manchas; com filtro ele já está liso.

## Cache de superfície realocando sem parar (2026-11-06, sombras do MyProject)
No jogo do usuário o cache refazia a alocação 10–12 vezes em 5 s, com a câmera parada.
- **Causa:** o log de mudanças do ECS guarda o **índice da slot**, e `needs_change` (`gi_cache.c`) o tratava como um
  handle de geração 0.
  - Numa slot já usada antes (geração 1 ou mais, como as dos atores refeitos), a entidade parecia destruída e com
    quadro.
  - Cada movimento dela pedia uma realocação (limitada a uma a cada 8 quadros), e cada realocação reamostra os quadros.
- **Correção:** `ecs_entity_at(slot)` pega o dono atual da slot. Agora há 1 realocação em 5 s. A cintilação do cache
  quase não mudou (1,65% → 1,62%): ela vem do histórico curto perto dos personagens animados, o que segue em aberto.

## AO de contato: o oclusor só é lido se estiver ao alcance (2026-11-06, casa assombrada)
Os personagens montados por osso ocupam as 32 vagas de oclusor (cada peça é Movable). Cada pixel copiava os 64 bytes
de cada oclusor antes de testar o alcance: 2,1 ms na casa com dois personagens.
- O alcance agora vai no `center.w` (negativo: esfera). `contact_gi` lê os 16 bytes do centro, testa e só então lê o
  resto (`occluder_light.comp` lê o tipo pelo sinal).
- Resultado: 2,1 → 1,3 ms.
- Reduzir o alcance de 4 para 3 raios não mudava nada: o custo era a leitura, não a conta das faces.

## A Delight piscando parada: duas sondas no mesmo slot (2026-11-07, vídeo do usuário)
Vídeo do usuário (MyProject, editor, câmera parada perto da Delight): a luz do rosto, do cabelo e do vestido dava
degraus de ~3-4% a cada ~50 ms, o fundo liso. Reproduzido no projeto dele com `--shot-every 1 --shot-skip 2000`
(`--shot-skip`: a sequência só depois de N quadros, com a história da GI já assentada).
- Bissecção: `--gi off` some; exposição manual não muda; sem TSR (`REALENGINE_NO_TSR=1`) o valor fica parado 3 quadros
  e salta no 4º — o ciclo das sondas (1/4 por quadro). A Delight é iluminada só pelas sondas (visão GI Source amarela).
  Congelar toda a história das sondas não resolvia; só deixar de reiniciar sondas resolvia.
- **Causa:** o slot toroidal `((cell % dims) + dims) % dims` com célula negativa. O SPIR-V é `OpSMod` (definido para
  negativos), mas no driver da NVIDIA o resultado saía errado quando o tamanho não é potência de 2 — só o z (12
  camadas; x e y têm 32). Com a câmera abaixo de 8 m o z da grade começa em -4, e as células -4..-1 (debaixo do chão)
  caíam nos slots das células 0..3 (a altura dos personagens). Cada uma achava a outra no slot e se reiniciava a cada
  atualização: a sonda do lado da Delight valia uma única leva de 64 raios, alternando com a de 8 m abaixo.
- **Correção:** `probe_slot` (`gi.glsl`) leva a célula para positivo antes do módulo (+65536 grades) e usa `uvec3`;
  `probe_update.comp` usa a mesma função. Afeta toda cena com sondas perto do chão (personagens, multidões, malhas sem
  quadro no cache); quem lia lightmap ou cache só via o efeito nos rebotes.
- **Checagem nova (toda medida):** um slot só guarda uma célula da grade por vez; o `probe_update.comp` conta (atômico,
  depois do último slot de `probe_state`) as atualizações que acham outra célula da grade no seu slot. No `--bench` a
  contagem é copiada para a CPU a cada quadro (4 bytes) e qualquer valor > 0 dá `FAIL: GI probes`. Com o módulo antigo
  a casa (`gi_house`) acusa 4,2 milhões; com o novo, 0. Fora das medidas não há cópia; o atômico só roda numa colisão.

Medidas (RTX 3060, 1440p, MyProject do usuário, câmera dele, editor):
| | antes | depois |
|---|---|---|
| Rosto da Delight, 28 quadros seguidos (luminância) | 79,9–84,0 em degraus | 69,25–69,29 |
| Vista que muda > 2% por quadro | 2,37–2,62% | 0,83–0,96% |
| Cintilação média | 0,133% | 0,09–0,125% |
| GPU mínima por quadro (GI) | 2,801 ms (0,265) | 2,800 ms (0,255–0,261): custo igual |
| Jogando (`--play`) | — | 0,01% vai e volta |

O que ainda pisca nessa vista (0,5% "vai e volta") não é a Delight: as linhas de eixo do editor (vermelha/verde, 1 px
com o jitter do TSR), a borda do chão no horizonte e as nuvens. `gi_house` 0,070–0,081% antes e 0,081% depois
(limite 0,1%).

## GI sem ray tracing por hardware: os raios por software (2026-11-09, plano de melhorias A1)
Numa GPU sem `VK_KHR_ray_query` (Android, Quest, AMD antes da RDNA 2, Intel antes da Arc; aqui com `--no-rt`) a GI
ficava só com o céu, os lightmaps e o volume baked: uma luz que se move não gerava indireta. Agora os raios da GI (o cache
de superfície, as sondas e a luz dos oclusores) percorrem BVHs em buffers (`src/sw_rt.c`, `shaders/sw_rt.glsl`), com as
mesmas chamadas do `rt.glsl` (`closest_in` / `visible_in` com `RE_SWRT`): os shaders `probe_trace`, `gi_cache` e
`occluder_light` ganham uma variante `_sw` e o resto da GI é o mesmo. `REALENGINE_NO_SWRT=1`: o comportamento antigo.
- **BVH por malha** (na carga e a cada recarga das malhas, na CPU): SAH com 16 baldes no eixo maior dos centroides, até
  4 triângulos por folha (8 quando o SAH não acha corte melhor), nós de 64 bytes com as caixas dos dois filhos, os
  triângulos (posições e o índice na malha) na ordem das folhas. `GpuMeshInfo.blas` guarda a raiz (| 1 << 32).
- **BVH das instâncias**: o conjunto estático refeito quando a cena muda (`scene_prepare` junta os atores como o
  `scene_rebuild`), o dinâmico a cada quadro (os `dyn_inst`, menos os skins: sem BVH da pose); cada folha leva o slot e
  a máscara (`RT_MASK_*`, como o `tlas_fill.comp`). Um buffer por slot de quadro; `FrameBuf.tlas` / `.tlas_dyn`
  guardam o endereço do cabeçalho de cada conjunto. A travessia: as caixas dos dois filhos, o mais perto por último na
  pilha (40), o raio para o espaço da malha pela inversa das linhas da instância.
- **Igual ao hardware:** só a GI (`--view gi`, house Médio), software × hardware: RMSE relativo 1,1%.
- Sem teste de alfa (um Masked bloqueia como opaco) e sem o path tracer nem a segunda vista (só por hardware).
- **Medidas** (RTX 3060, `--no-rt`, Médio): house 398 fps (meta 200; antes 567 sem GI nenhuma), GI 0,50 ms, cintilação
  0,072%; RPG 130 fps (antes 362 sem GI), GI 2,6 ms, cintilação 0,038%. Os atores em movimento: 8K texels por quadro no
  software (98K no hardware): na RPG os 23K por quadro eram 2,5 dos 3,4 ms. BVHs: house 19,6 MB em 85 ms, RPG 24,5 MB
  em 168 ms.
- **Orçamento adaptativo** (render.c, antes do `gi_fill_frame`): os texels do cache por quadro e os raios das sondas
  escalados para manter a GI perto de 2 ms (o tempo medido, como a resolução dinâmica; desce até 1/4 por quadro, sobe
  10%). House e RPG ficam com quase tudo. As BVHs por software saem no worker que lê as malhas (`swrt_prepare`), a GPU
  só recebe (`swrt_commit`): a heavy levava 1 s na thread principal a cada recarga.
- **O limite: folhagem densa.** A heavy (2,1 M triângulos de cards, Épico, `--no-rt`): 172 ms de GI por quadro no
  orçamento do preset; com o adaptativo ~50 fps (o cache no piso de 1-2K texels: cada raio nascido na folhagem percorre
  muitas folhas sobrepostas). Um LOD mais leve na BVH ficou **mais lento** (buracos alongando os raios: 2 fps) e foi
  descartado. O teste `heavy_mesh` (o raster das malhas pesadas) roda com `REALENGINE_NO_SWRT=1`, como media antes.
- **Falta:** teste de alfa nos Masked, a BVH da pose dos skins, o refit em vez de rebuild do conjunto dinâmico, medir num
  celular (o orçamento por preset de escala), a água (A1e) e o SDF do A1b.

## Sondas distantes: a segunda cascata (2026-11-09, plano de melhorias A2b/A2d)
Além da grade de sondas perto da câmera (32 × 32 × 12 a 2 m: ~64 m) só havia a luz do céu: um prédio ou uma colina a
100 m não recebia rebate nem oclusão. Agora há uma **segunda grade** com as mesmas sondas a 8× o espaçamento (16 m:
~512 m × 512 m × 192 m), centrada na câmera como a primeira (`gi.glsl probe_grid(f, 1)`: seus parâmetros saem do
`FrameBuf` da primeira e da câmera, sem campo novo). Os ladrilhos dela ficam embaixo dos da primeira no atlas (o atlas
tem o dobro da altura), o estado depois do da primeira (e do contador dos slots), os raios na metade de cima do buffer.
- **Atualização:** uma sonda em 16 por quadro (a luz de longe muda devagar), no mesmo `probe_trace`/`probe_update`
  com `pc.pad = 1`.
- **Leitura:** `probe_irradiance` lê a grade perto; fora dela, e nas suas 2 últimas sondas misturadas, a de longe (com o
  teste de visibilidade dela e os vieses na escala dela).
- **Só onde ajuda:** liga quando a cena estática passa da grade perto em volta da câmera (render.c, pelos limites dos
  atores que projetam sombra); a house cabe nela e não paga nada. `REALENGINE_NO_FAR_PROBES=1` desliga. O log diz
  "Sondas distantes (A2): ligadas / desligadas".
- **Medidas** (3060, Épico): RPG (vila, câmera do `demo_rpg_flicker`) 110-116 × 115 fps, GI +0,08 ms, cintilação igual
  (0,07%); landscape 143 × 154 fps (+0,45 ms: o passe principal lendo duas grades onde a vista é longe). Memória: +14,7
  MB de atlas e +0,4 MB de estado.
- Teste `far_probes` (landscape, validação, ligadas, cintilação < 0,1%).
- **Falta (A2):** a) quadros por face nos cubos; c) atlas virtual; d) relocação das sondas (DDGI) e o filtro espacial;
  e) coleta a meia resolução; f) o ponto de 2 cm da house_thin; g) tecido e oceano na GI.

## A "caixa de sombra" embaixo dos personagens (2026-11-14, relato do usuário)
"Os personagens ainda estão com uma caixa de sombra embaixo deles": um quadrado escuro no chão sob a Delight, e um
quadrado mais claro quando os pés afundam um pouco no chão. Causa: o AO de contato usava a primeira pele (e as peças
grandes) da Skeletal Mesh como oclusor de **caixa** — a caixa da pose de repouso (braços abertos, a saia inteira). A
fórmula exata da caixa desenha um quadrado; dentro dela nenhuma face olha para o chão (o claro). Agora peles e peças
(`CAST_SHADOW_ROUND_OCCLUDER` → `INST_ROUND_OCCLUDER`, e qualquer `INST_SKINNED`) viram uma cápsula ao longo do eixo mais
em pé: duas esferas (uma, se curta) com raio de 0,6 × o lado mais fino. Mancha suave que segue o corpo, sem quadrado.

## ReSTIR GI: reuso espacial dos raios do denoiser (2026-11-15, "vamos fazer a parte do GI e Reflexos")
No Fotorrealista/Vídeo-realista (denoiser unificado), Alto para cima (`gi.restir_gi`; `REALENGINE_NO_RESTIR_GI=1` desliga).
- `den_trace.comp` passe 0 guarda as amostras dos dois raios de cada pixel (difuso e especular áspero: direção, radiância
  pré-exposta, distância do acerto, W = 1/pdf, M; 32 B por pixel de render, `R.rgi`, `FrameBuf.rgi`). Passe 1 (novo,
  "denoiser: ReSTIR GI espacial") junta as de 2 vizinhos a até 16 px no mesmo plano (o teste de plano do `denoise.comp`):
  o ponto atingido pelo vizinho visto daqui (direção, distância, Jacobiano da distância², rejeita fora de ¼..4×), e um
  raio de visibilidade só onde os dois pontos podem ver diferente (o vizinho a mais de 15% da distância do acerto, e o
  acerto a menos de 4 m). Alvo: luminância × cosseno/π (difuso) e luminância × pdf do GGX visível daqui (especular).
- **Estimador Rao-Blackwell**: o sinal é a soma ponderada de todos os candidatos, (1/M) Σ wᵢ Lᵢ / lum(Lᵢ), não um
  sorteado entre eles (o sorteio mudava a cada quadro: ferveu). Os pixels do xadrez sem raio neste quadro são
  preenchidos pelos vizinhos (antes o denoiser os preenchia).
- **O reuso temporal do artigo foi testado e ficou de fora**: por cima do histórico do próprio denoiser a imagem
  fervia (house 0,151% → 0,17-0,25% de cintilação, limite M 20, 8 e 3); só o espacial: 0,131%.
- Medida (house, Fotorrealista, Épico, 1440p, render 1617×998, RTX 3060): cintilação 0,151% → 0,131%; GI difusa sem as
  manchas no chão e nas paredes, cantos mais escuros como devem; reflexo áspero sem as manchas nas paredes e no teto (o
  chão áspero melhora pouco). Custo: passe espacial 0,50 ms só difuso (0,62 sem a visibilidade; o vizinho lendo o
  `o_den` de 128 bits custava 0,15), 0,79 ms com o especular; quadro 6,37 → ~6,9 ms.

## Build Lighting em jogo (Q16, 2026-11-18)
O mesmo bake rodando no jogo para mapas gerados em jogo: modos Loading Screen e Progressive (faixas do atlas no orçamento
medido pelos timestamps 28–29, sondas do bake em passo 4, lightmaps escondidos até o fim, início numa thread e envio em
fatias). Detalhes e medidas: [em-jogo.md](em-jogo.md).

## GI de fallback sem raios: AO em volume e luz rebatida do chão (Q27.12, 2026-11-23: a patente US 12,263,404 do GTA 6)
- **Onde vale**: no `mesh.frag`, só nas variantes sem RT (as do Android, do Quest, das GPUs sem ray query: as RT não
  mudam de registradores) e só onde nada mais ilumina o difuso (sem lightmap, sondas ou cache: `!gi`).
- **AO em volume** (`gi_fallback.c`): 64 × 64 × 16 células de 2 m em volta da câmera (128 × 128 × 32 m; a grade anda de
  16 em 16 m, um quarto dela abaixo da câmera), um byte de visibilidade do céu por célula. Feito numa thread quando a
  câmera anda 16 m ou a cada 2 s, das caixas (AABB no mundo) das malhas estáticas e móveis: a **opacidade** de uma caixa
  numa célula é o produto das duas maiores sobreposições por eixo (uma parede ou laje de 20 cm que cruza a célula para o
  raio como um bloco; a ocupação por volume deixava as paredes finas transparentes); a visibilidade: 5 raios (para cima
  0,4; quatro a 45° 0,15 cada) de até 10 células, transmitância Π(1 − 0,9·opacidade). Buffer com endereço em
  `water[24..25]` (os slots livres da água), `GI_FALLBACK`.
- **Luz rebatida do chão**: o sol (o primeiro Directional: intensidade × cor) na altura dele × albedo 0,2 / π
  (`water[26]`). `irr = céu × vis + rebatida × ((1 − vis) × 0,8 + (0,5 − 0,5·n.z))`: o que olha para o lado e para baixo
  vê o chão; a parte do céu escondida vê as superfícies em volta, iluminadas pelo rebote (sem isso, sob um telheiro
  ficava preto e as paredes ao pé dos prédios escuras demais, contra o RealGI). 0,8 foi o melhor de 0,3 / 0,55 / 0,8 / 1,0.
- **Comparação por imagem** (`tools/gi_fallback_compare.py`, cena `gi_fallback`: telheiro, bloco, muro; `--view gi
  --game-view --manual-exposure`: com exposição automática o tone map muda a imagem toda e a comparação não vale):
  diferença relativa para o RealGI (Médio, raios por software) **10,3 % sem o fallback → 9,2 % com**; sob o telheiro
  (125, 146, 174) no RealGI, (99, 143, 190) sem, (118, 145, 179) com.
- **Custo** (RTX 3060, 1440p, Baixo, `--no-rt`): passe principal 0,244 → 0,260 ms (**+0,016 ms**, meta 0,3 ms no
  Android); a construção 5-6 ms numa thread, fora do quadro. Teste `gi_fallback` (área gi): o volume construído, 200 fps.
- Fica para depois: medir num celular e no Quest, o volume das malhas pela geometria (a caixa de uma casa oca é
  cheia: o interior sem céu é certo, mas uma casa aberta não), a cor do chão em vez de 0,2, o RT por hardware no Baixo.

## A GI vê o brilho dos materiais (2026-11-23)
- `GpuInstance.emissive_rough.z` de uma instância com material leva a **emissão média** em nits (`render.c
  render_emis_pack`: 10 bits por canal em log2, passo de 1/32 de oitava a partir de 2^-10 nits) e o bit 30 quando o ator
  já ilumina como luz pontual (notas-dev/luz.md). `common.glsl inst_emission/inst_glow` lêem; o `Hit` ganhou `glow`.
- **Difusa** (cache de superfície, raios do denoiser, sondas, lightmap, contato, path tracer depois do 1º hit): soma
  `h.glow` — a emissão de quem **não** tem a luz pontual (quem tem: a luz já leva, com sombra; os dois contariam duas
  vezes). **Reflexos e o que a câmera vê** (`mesh.frag` reflexo por raio, água, 1º hit do path tracer): `h.emissive`
  inteiro. Antes era 0 para todo material.
