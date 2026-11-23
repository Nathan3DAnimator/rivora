# Tecnologia do GTA 6: cidade densa, prédios, vegetação, água e streaming de dados

Data da pesquisa: 23/11/2026. O GTA VI saiu em **19/11/2026** (PS5 e Xbox Series X|S; PC não anunciado, rumor de 1º tri. 2027) — [Wikipedia](https://en.wikipedia.org/wiki/Grand_Theft_Auto_VI); [GTA BOOM (DF, 19/11/2026)](https://www.gtaboom.com/digital-foundry-finds-that-gta-6-indeed-runs-best-on-ps5-pro-b72f).

Legenda de confiança: **[C]** confirmado (Rockstar/Take-Two, ou medido na versão lançada); **[A]** análise de especialista (Digital Foundry etc.); **[E]** especulação/fã/forum/rumor; **[I]** inferência minha (implementação provável e equivalente na UE5).

Aviso de fontes: o artigo/vídeo original da Digital Foundry (digitalfoundry.net, ~18/11/2026) não pôde ser aberto (403 nos espelhos de fórum); tudo da DF abaixo vem de cobertura secundária (TweakTown, GTA BOOM, libertycity, GTA Intel, playfront). Nenhum GDC/SIGGRAPH da Rockstar sobre o GTA VI foi encontrado — a Rockstar não publica postmortems técnicos.

## 1. Tamanho do mapa, densidade, interiores, LOD/HLOD/impostores, distância de visão, materiais

### Takeaway
Oficialmente só se sabe que Leonida é "cerca de duas vezes" o mapa do GTA V, mais denso e com "muito mais interiores"; a Rockstar nunca publicou área, número de interiores, distância de visão nem faixas de LOD. A única pista técnica dura sobre LOD é a patente de Continuous LOD da Take-Two (quantização de vértices + máscara de bits, blending entre níveis), que é o oposto conceitual do Nanite (simplificação por grade de quantização, não clusters em DAG).

### Cited Findings
- [C] Aaron Garbut (Game Informer, set/2026): Leonida tem "about twice the size" do mapa do GTA V, "denser, more detailed, and more interactive, with a lot more interiors"; seis regiões: Vice City, Leonida Keys, Grassrivers, Port Gellhorn, Ambrosia, Mount Kalaga National Park — [GTA BOOM (29/09/2026)](https://www.gtaboom.com/everything-new-we-learned-about-gta-6-from-the-game-informer-cover-story-4d76)
- [C] Garbut: "Normally you compromise scale for detail or detail for interaction, but with this project we went the other way: everything is dialed up..."; "critical mass with the numbers of our interiors"; Vice City é a região mais densa — [TheGamer](https://www.thegamer.com/gta-6-map-so-massive-story-doesnt-cover-it-all-game-informer-interview/); [GTA Series Videos no X](https://x.com/GTASeries/status/2104975449969340786)
- [C/A] Wikipedia: Leonida ~3x a área jogável do RDR2, cinco condados (Vice-Dale, Kelly, Mariana, Leonard, Lummox) — [Wikipedia](https://en.wikipedia.org/wiki/Grand_Theft_Auto_VI) (afirmação de terceiros; não confirmei a fonte primária)
- [E] Área em milhas² sem número oficial: estimativas da comunidade vão de ~50 mi² a 118-132 mi² (ou ~175 km² contando água) — [Gfinity (mapa)](https://www.gfinityesports.com/article/how-big-is-the-gta-6-map-leonida-size-and-confirmed-locations)
- [E] "700+ prédios com interior" circula, mas não há contagem oficial nem percentual — [The News Edge](https://thenewsedge.com/gta-6s-leonida-map-2-7x-larger-than-gta-5-700-enterable-buildings/); [Gfinity (mapa)](https://www.gfinityesports.com/article/how-big-is-the-gta-6-map-leonida-size-and-confirmed-locations)
- [C, review] Review com 3 semanas de jogo (PS5 Pro): "far more interiors compared to previous games", "that doesn't mean all of them are accessible"; densidade "like never before" (NPCs, trânsito, eventos aleatórios) — [Gfinity review (18/11/2026)](https://www.gfinityesports.com/article/gta-6-review-worth-every-second-of-the-13-year-wait)
- [C] Rockstar nunca publicou número de distância de visão, faixa de LOD nem lista de opções gráficas — [GTA Intel (draw distance)](https://gtaintel.com/news/gta-6-draw-distance-and-world-detail)
- [A/E] Observado em footage (não medido): prédios, carros e palmeiras distantes continuam detalhados; muito mais props de rua (lixeiras, vasos, placas, carros parados); interiores visíveis pelas janelas com aparência de interior mapping por parallax (técnica não descrita pela Rockstar) — [GTA Intel (draw distance)](https://gtaintel.com/news/gta-6-draw-distance-and-world-detail)
- [C, patente] US 11,978,162 B2 "System and method for improved rendering of computer graphic models", Take-Two, prioridade 18/12/2020, concedida 07/05/2024 (pub. US 2022/0198752 A1): continuous LOD — vértices quantizados em inteiros sem sinal; máscaras zeram bits baixos para gerar níveis mais grossos; triângulos que colapsam (vértices iguais) são removidos; listas ordenadas de vértices/triângulos compartilhadas entre níveis com tabela de quais entradas cada nível usa; LOD em float calculado por distância, resolução, FOV e escala; interpolação ponderada entre dois níveis para evitar popping; dequantização no render — [Google Patents US11978162B2](https://patents.google.com/patent/US11978162B2/en)
- [A] A DF elogiou detalhe de ambiente altíssimo (garrafas como exemplo) e o iguana do trailer de lançamento, mais detalhado que o de Cyberpunk 2077 e o sapo tesselado do Crysis 3 — [libertycity (DF trailer 2, 08/05/2025)](https://libertycity.net/news/gta-6/8997-digital-foundry-analyzes-gta-6s-second.html); [GTA Intel (DF launch trailer, 16/10/2026)](https://gtaintel.com/news/gta-6-launch-trailer-digital-foundry-analysis)
- [A] Versão lançada: 1440p interno em PS5, PS5 Pro e Series X; Pro reconstrói com PSSR a "near-4K", PS5/Series X com upscale espacial + TAA próprio da Rockstar; Series S 720p interno com FSR para ~1080p; 30 fps em todos, sem modo 40/60 — [GTA BOOM (DF)](https://www.gtaboom.com/digital-foundry-finds-that-gta-6-indeed-runs-best-on-ps5-pro-b72f); [TweakTown (DF, 18/11/2026)](https://www.tweaktown.com/news/114763/gta-6-deemed-the-single-most-visually-striking-game-of-the-generation-by-tech-experts/index.html)
- [A] RTGI e reflexos por RT em todos os consoles (inclusive Series S); sombras por RT só no PS5 Pro (no PS5 base há "chunky shadow artifacts") — [TweakTown](https://www.tweaktown.com/news/114763/gta-6-deemed-the-single-most-visually-striking-game-of-the-generation-by-tech-experts/index.html); [GTA BOOM](https://www.gtaboom.com/digital-foundry-finds-that-gta-6-indeed-runs-best-on-ps5-pro-b72f)
- [A] Trailer 2 (DF, mai/2025): iluminação inteira baseada em RTGI; reflexos por RT em rios e corpos d'água pequenos, SSR em superfícies grandes de água, vidro e plástico; sombras filtradas (não RT), como no RDR2; build então a 1152p com FSR1 (depois superado pelo 1440p medido no lançamento) — [libertycity](https://libertycity.net/news/gta-6/8997-digital-foundry-analyzes-gta-6s-second.html)
- [A] O limite é a CPU (Zen 2), não a GPU: a simulação densa (NPCs, IA, animação, trânsito) segura o PS5 Pro em 30 fps; um modo 40 fps exigiria cortar multidões e trânsito (33,3 → 25 ms) — [TweakTown](https://www.tweaktown.com/news/114763/gta-6-deemed-the-single-most-visually-striking-game-of-the-generation-by-tech-experts/index.html); [Wikipedia](https://en.wikipedia.org/wiki/Grand_Theft_Auto_VI)
- [C/A] Mais de 600 mil animações únicas de NPC (300 mil no RDR2, 55 mil no GTA V), retargeting em tempo real entre corpos de proporções diferentes para poupar memória; NPCs feitos à mão, não procedurais — [Wikipedia](https://en.wikipedia.org/wiki/Grand_Theft_Auto_VI)
- [A] Pior caso medido: PS5 base cai a 23 fps em explosões pesadas; PS5 Pro 24 fps; Series X ~20-22 fps em tiroteios pesados — [TweakTown](https://www.tweaktown.com/news/114763/gta-6-deemed-the-single-most-visually-striking-game-of-the-generation-by-tech-experts/index.html); [GTA BOOM](https://www.gtaboom.com/digital-foundry-finds-that-gta-6-indeed-runs-best-on-ps5-pro-b72f)
- [E] Forum sobre o review da DF: um usuário notou pop-in de LOD na folhagem em footage da BBC; outro cita um reviewer que não viu nenhum pop-in — [ResetEra](https://www.resetera.com/threads/grand-theft-auto-6-digital-foundry-tech-review-the-game-of-the-generation.1690775/page-17)
- [A, via X de fã] Detalhes finos às vezes "soft" ou com shimmer à distância — [GTA 6 Info no X](https://x.com/GTASixInfo/status/2093354805100269899) (não conferido contra a DF)
- [A] Sujeira de reconstrução/AA visível em grades e cercas; franjas violeta ao redor de água e luzes (tone mapping HDR; DF/Filippo Tarpini suspeitam do AgX) — [playfront (18/10/2026)](https://playfront.de/en/gta-6-digital-foundry-analysiert-rage-engine-und-ps5-performance/); [GTA Intel (DF launch trailer)](https://gtaintel.com/news/gta-6-launch-trailer-digital-foundry-analysis)

### Inferences
- [I] **LOD**: a patente CLOD indica malhas tradicionais (não micropolígonos) com níveis gerados por quantização e transição contínua (geomorph). Equivalente UE5: LODs de Static Mesh com dithered LOD transition, ou Nanite para a geometria opaca. Diferença: Nanite troca clusters numa DAG por erro em pixels; o CLOD da Take-Two é um "vertex clustering" em grade com morph — barato, sem custo de visibility buffer, bom para CPU/GPU de console a 30 fps. Para a Real Engine (que já tem LOD de clusters em DAG, `render.md`), o ganho a copiar é o *morph entre níveis* para zerar popping, não a estrutura.
- [I] **Prédios distantes**: o RAGE histórico (GTA V/RDR2) usa hierarquia de LOD por "containers" (HD → LOD → SLOD1..SLOD4, malhas de quarteirão fundidas); a ausência de "blocos chapados" no fundo sugere que continua assim, com proxies combinados por região. Equivalente UE5: **HLOD do World Partition** (Merged/Simplified/Approximated meshes por célula). Para vegetação distante, impostores (Equivalente UE5: Impostor Baker / HLOD com impostores). Não há confirmação de impostores no GTA VI.
- [I] **Janelas**: "interiores visíveis pelas janelas" em prédios não-acessíveis quase certamente é interior mapping (cubemap/atlas de sala por parallax no shader), como o RDR2/Spider-Man. Equivalente UE5: material com `InteriorCubemap` / Matrix Awakens. A Real Engine já tem Interior Mapping (`materiais.md`).
- [I] **Interiores acessíveis**: a "massa crítica de interiores" sem tela de loading em PS5 implica interiores como células de streaming próprias com portal/occlusion (o RAGE usa "MLO interiors" com portais e salas desde o GTA IV). Equivalente UE5: Level Instances + World Partition com Data Layers, ou Level Streaming por volume.
- [I] **Densidade a 30 fps limitada por CPU** é o ponto principal para a Real Engine: o GTA VI gasta orçamento de CPU em simulação (IA, animação, trânsito), não em render — a meta de 200 fps da Real Engine exige o caminho inverso (ECS SoA + jobs, multidões em compute, como já em `multidao.md`).
- [I] **Iluminação**: RTGI + reflexos RT (rios) + SSR (oceano/vidro) + sombras por mapa filtradas (RT só no Pro) é um híbrido muito parecido com Lumen (HW RT) + SSR + Virtual Shadow Maps. Equivalente UE5: Lumen Hardware RT em modo "Scalability High (30 fps)".

### Gaps
- Nenhuma contagem oficial de interiores acessíveis, nem % de prédios.
- Nenhuma área oficial em km²/mi²; as estimativas divergem 2-3x.
- Nenhuma medição de distância de visão ou de transição de LOD na versão lançada (o vídeo da DF não foi lido diretamente).
- Decals, sujeira, wetness de materiais: só há confirmação de poças persistentes (seção 3); nada sobre o sistema de decals/dirt.
- Nenhuma confirmação de instancing GPU-driven, mesh shaders, visibility buffer ou Nanite-like no RAGE 9ª geração.

## 2. Vegetação (palmeiras, pântanos tipo Everglades), vento, densidade e LOD de folhagem

### Takeaway
Confirmado: sistema de vento "heavily upgraded" que move vegetação (capim do pântano dobrando), clima localizado e persistente, Grassrivers (Everglades) com barro e água rasa. Detalhes de implementação (LOD/impostores de palmeira, densidade) não foram publicados; a única crítica técnica é shimmer na folhagem fina.

### Cited Findings
- [C] Clima "re-engineered" para ser localizado e persistente ("sunny where you are while a storm hits somewhere nearby"), movido por um "heavily upgraded wind system"; capim do pântano dobra com o vento; poças persistem depois da chuva; arco-íris — [GTA BOOM (Game Informer, 29/09/2026)](https://www.gtaboom.com/everything-new-we-learned-about-gta-6-from-the-game-informer-cover-story-4d76)
- [C] Nuvens volumétricas que se movem pelo mapa, furacões e tempestades tropicais com chuva localizada e visibilidade reduzida — [Wikipedia](https://en.wikipedia.org/wiki/Grand_Theft_Auto_VI)
- [C] Mais de 170 espécies de animais (60+ aves, 30+ mamíferos terrestres; tubarões, baleias, raias, tartarugas, águas-vivas) — [GTA BOOM (Game Informer)](https://www.gtaboom.com/everything-new-we-learned-about-gta-6-from-the-game-informer-cover-story-4d76)
- [C, review] Mount Kalaga: trilhas, "remote forests, rivers"; Grassrivers: "damp, muddy terrain"; incêndios florestais no interior; névoa matinal — [Gfinity review](https://www.gfinityesports.com/article/gta-6-review-worth-every-second-of-the-13-year-wait)
- [A] Palmeiras e árvores continuam legíveis ao fundo em praias e estradas, sem virar "blocos chapados" (observação de footage) — [GTA Intel (draw distance)](https://gtaintel.com/news/gta-6-draw-distance-and-world-detail)
- [A] Shimmer em bordas finas e folhagem no footage, que o PSSR ajuda a limpar (citado como de outro texto da DF) — [TweakTown](https://www.tweaktown.com/news/114763/gta-6-deemed-the-single-most-visually-striking-game-of-the-generation-by-tech-experts/index.html)
- [E] Pop-in de LOD na folhagem notado por um usuário em footage — [ResetEra](https://www.resetera.com/threads/grand-theft-auto-6-digital-foundry-tech-review-the-game-of-the-generation.1690775/page-17)

### Inferences
- [I] **Vento**: um campo de vento global com rajadas regionais (que o clima localizado exige) amostrado no vertex shader da vegetação (pivô por galho/folha, como o "Pivot Painter" ou os dados de vértice do SpeedTree). Equivalente UE5: SpeedTree wind / Pivot Painter 2 + `WindDirectionalSource`; para clima regional, um Wind field em textura 2D (como o Fluid/Wind do Water plugin ou soluções próprias). A Real Engine já tem vento no Foliage (`landscape.md`); o que falta é o vento *localizado* por tempestade.
- [I] **Densidade/LOD de folhagem**: o provável é instancing por célula com LOD discreto + dither/morph e cartões/impostores no anel distante. Equivalente UE5: Foliage/HISM + Nanite Foliage (5.6+, voxelizada) ou PCG + impostores. Na Real Engine: impostores octaedrais já existem (`visao-distante.md`).
- [I] **Pântano**: capim alto + água rasa com reflexo RT (rios usam RT segundo a DF) e ondulações geométricas — ver seção 3.

### Gaps
- Nenhuma fonte sobre contagem de instâncias, distância de folhagem, técnica de LOD das palmeiras, ou uso de SpeedTree.
- Nenhuma fonte confirmando vegetação destrutível/interativa (amassar ao passar com carro) no GTA VI.

## 3. Água: oceano, praias, espuma, rastros de barco, pântano, poças, debaixo d'água, flutuação

### Takeaway
Confirmado em footage oficial do PS5: oceano com ondas que mudam com o clima (swells grandes em tempestade), cor por profundidade, recife com cáusticas, mergulho/snorkel, airboat, jet ski, caiaque; ondulações geométricas ao andar no pântano; poças persistentes com reflexo e respingo. As afirmações de "física de água revolucionária" (orçamento de US$ 200-300 mi, enchentes em tempo real) são rumor; as patentes de fluido da Take-Two existentes são de 2012-2020 (simulação 2D de fluido projetada em superfícies 3D), não comprovadamente no jogo.

### Cited Findings
- [C] Extended Look (27/08/2026), "captured entirely from in-game footage on the PlayStation 5": airboats no pântano, jet skis, caiaque, snorkel e mergulho — [GTA Intel (água, atualizado 12/09/2026)](https://gtaintel.com/news/gta-6-water-physics-and-ocean-tech)
- [C/A] Cor por profundidade (azul escuro no fundo, turquesa no raso, fundo de areia visível); água calma com céu limpo, picada com nublado, swells muito maiores nas tempestades do trailer 2; recife com tartarugas, moreias, corais, luz filtrando e cáusticas no fundo — [GTA Intel (água)](https://gtaintel.com/news/gta-6-water-physics-and-ocean-tech)
- [A] Personagem andando no pântano deixa ondulações geométricas atrás; gotas no chuveiro iluminadas pelo sol "unusually good" — [GTA Intel (DF launch trailer)](https://gtaintel.com/news/gta-6-launch-trailer-digital-foundry-analysis)
- [A] Reflexos por RT em rios e pequenos corpos d'água; SSR nas superfícies grandes de água — [libertycity (DF trailer 2)](https://libertycity.net/news/gta-6/8997-digital-foundry-analyzes-gta-6s-second.html)
- [A] Franjas violeta ao redor de superfícies de água atribuídas ao tone mapping HDR — [playfront](https://playfront.de/en/gta-6-digital-foundry-analysiert-rage-engine-und-ps5-performance/)
- [C, review] Ondas batendo na praia com "remarkable physics and audio"; poças "catch realistic reflections and splash with uncanny accuracy"; som da chuva muda conforme a superfície — [Gfinity review](https://www.gfinityesports.com/article/gta-6-review-worth-every-second-of-the-13-year-wait)
- [C] Poças persistentes depois de chuva forte; furacões e tempestades tropicais confirmados — [GTA BOOM (Game Informer)](https://www.gtaboom.com/everything-new-we-learned-about-gta-6-from-the-game-informer-cover-story-4d76); [Wikipedia](https://en.wikipedia.org/wiki/Grand_Theft_Auto_VI)
- [E] Alturas de onda, marés dinâmicas, surfe, correntezas, cálculo de flutuação de barcos, enchentes e "sparse fluid simulation" patenteada no jogo: não confirmados — [GTA Intel (água)](https://gtaintel.com/news/gta-6-water-physics-and-ocean-tech)
- [E] "US$ 200-300 milhões só em física de água" — rumor sem fonte primária — [Tech4Gamers](https://tech4gamers.com/gta-6-most-realistic-water-physics/)
- [C, patente] US 9,922,141 (Take-Two, concedida mar/2018; inventores Rowan Wyborn, Mathi Nagarajan) e continuação US 10,685,152 "fast simulation and visualization of sparse fluids": fluido calculado num plano 2D particionado que representa a superfície 3D, fluxo por partição sob vetor gravidade, renderizado de volta na superfície 3D (exemplo: suor/água em rostos de jogadores de basquete, com roll-off e liftoff) — [Google Patents US10685152](https://patents.google.com/patent/US10685152); [Justia (Take-Two)](https://patents.justia.com/assignee/take-two-interactive-software-inc?page=2)
- [C, patente] US 8,237,722 (Take-Two, 2012, Jesse Johnson) "visualization of fluids": chuva caindo simulada como plano 2D e malha de fluido + respingos na superfície de um objeto 3D — [uspto.report](https://uspto.report/patent/grant/8,237,722)

### Inferences
- [I] **Oceano**: o padrão da indústria para ondas que mudam com o vento é espectro FFT (Phillips/JONSWAP) em cascatas + deslocamento Gerstner/FFT na praia, com espuma por Jacobiano e por textura de distância à costa. Equivalente UE5: **Water plugin** (Water Body Ocean com Gerstner waves, Water Zone, shoreline via distance field) — note que o Water da UE5 usa Gerstner, não FFT, e não tem swell por clima regional nativo. A Real Engine já tem oceano FFT com espuma e rastros (`agua.md`); o que o GTA VI acrescenta é o acoplamento *clima regional → espectro*.
- [I] **Cor por profundidade e cáusticas**: absorção por profundidade (Beer-Lambert) com depth buffer + textura de cáusticas animada projetada no fundo. Equivalente UE5: Single Layer Water shading model + Caustics da Water Zone.
- [I] **Pântano/rios com RT e oceano com SSR**: decisão de orçamento — superfícies pequenas e planas recebem raios; o oceano (enorme, ondulado) usa SSR + fallback de sonda/céu. Útil para a Real Engine: RT por área de água em vez de global.
- [I] **Ondulações ao andar/barco**: simulação de altura 2D local (wave equation em textura ao redor do jogador/barcos), como o Water Body "Fluid Simulation" do plugin de exemplo da UE5 (WaterFluidSim / Niagara Fluids 2D). A patente sparse fluid mostra que a Take-Two já domina simulação 2D projetada em superfícies.
- [I] **Poças persistentes**: máscara de wetness/poças em textura mundial (virtual texture ou clipmap) acumulada pela chuva e evaporada com o tempo, lida pelo material (como o RVT da UE5). Equivalente UE5: Runtime Virtual Texture + Material Parameter Collection (o "Wetness" do Matrix Awakens). A Real Engine já tem superfícies molhadas em duas camadas (`agua.md`); a novidade seria a *persistência por região* do clima localizado.
- [I] **Flutuação**: provavelmente pontos de flutuação amostrando a altura da onda na CPU (como o Buoyancy da UE5), já usados no GTA V. Sem confirmação.

### Gaps
- Nenhuma fonte técnica sobre espuma de praia, rastro de barco, tipo de espectro de onda ou flutuação na versão lançada.
- O review da DF sobre água não foi lido diretamente.
- Sem fonte primária ligando as patentes de fluido (feitas para jogos de esporte 2K) ao GTA VI.

## 4. Streaming de dados e memória no PS5 (SSD/Kraken, velocidade ao dirigir rápido), orçamento de assets, patentes

### Takeaway
O único número duro é o tamanho: até **212 GB** instalado no PS5 (Rockstar, 03/11/2026), ~209,75 GB de download; loadings de 5-10 s no review. Não há fala oficial nem medição da DF sobre throughput do SSD, uso de Kraken/Oodle ou pop-in em alta velocidade. As patentes recentes da Take-Two/Rockstar cobrem continuous LOD (2024), navegação de NPC por grafo de ruas e tempo de carregamento de sessões online — nada encontrado sobre geração procedural de terreno/mapa.

### Cited Findings
- [C] Rockstar (03/11/2026): até 212 GB de armazenamento conforme o aparelho; ~209,75 GB de download no pré-load; caixa alemã do PS5 indica 230 GB livres — [Push Square (nov/2026)](https://www.pushsquare.com/news/2026/11/gta-6-ps5-file-size-confirmed-to-be-212gb-physical-map-available-but-not-in-boxed-copies); [RockstarINTEL](https://rockstarintel.com/gta-6-pre-load-begins-rolling-out-updated-install-size-revealed/); [timesaver.gg](https://timesaver.gg/blog/gta-6-file-size-storage-ps5-xbox)
- [E] Xbox Series: ~209,86 GB download, 225,34 GB instalado (fontes divergem) — [stealthygaming](https://stealthygaming.com/how-many-gb-will-gta-6-be/)
- [C, review] "Load times were very fast", ~5-10 s na tela de entrada; sem crash/freeze — [Gfinity review](https://www.gfinityesports.com/article/gta-6-review-worth-every-second-of-the-13-year-wait)
- [A] É o primeiro jogo da Rockstar feito especificamente para a 9ª geração (sem PS4/Xbox One nativo; Xbox One só por cloud) — [Wikipedia](https://en.wikipedia.org/wiki/Grand_Theft_Auto_VI)
- [A] Series X: saída 120 Hz com VRR e LFC; PS5 base só usa VRR acima de 48 fps — [GTA BOOM (DF)](https://www.gtaboom.com/digital-foundry-finds-that-gta-6-indeed-runs-best-on-ps5-pro-b72f)
- [C, patente] US 11,978,162 B2 (continuous LOD, concedida 07/05/2024) — ver seção 1 — [Google Patents](https://patents.google.com/patent/US11978162B2/en)
- [C, patente] US 11,684,855 B2 descrita como navegação mais suave de NPC por grafo da rede de ruas — [Wccftech (22/05/2025)](https://wccftech.com/grand-theft-auto-vi-patents-revolutionize-open-world-forever/); [VGC](https://www.videogameschronicle.com/news/gta6-npc-improvements-patent/)
- [E] Recap de 10 patentes da Rockstar (via GTA6Alerts no X): personagens realistas, fluido em superfícies 3D, navegação de NPC por grafo de ruas, render melhorado, continuous LOD, redução de tempo de carregamento de sessões online, predição de pose (mocap + IA), transações, blocos de animação data-driven, anti-cheat — sem números nem datas no artigo — [Wccftech](https://wccftech.com/grand-theft-auto-vi-patents-revolutionize-open-world-forever/)

### Inferences
- [I] 212 GB para um mapa ~2x o GTA V (GTA V PS5 ~ 80-90 GB) é consistente com texturas de alta resolução *sem* duplicação de dados para HDD (o PS5 dispensa layouts duplicados) e com compressão Kraken por hardware — mas a Rockstar não confirmou Kraken/Oodle. Equivalente UE5: Oodle Kraken/Texture + IoStore/Zen loader + **World Partition** streaming por célula com prioridade pela velocidade do jogador.
- [I] Dirigir/voar rápido em 30 fps com RTGI exige streaming preditivo pelo vetor de velocidade (o RAGE já faz isso desde o GTA V: carros e aeronaves rápidos reduzem densidade de trânsito e trocam para LODs mais baixos). Equivalente UE5: World Partition Streaming Sources com velocidade + `wp.Runtime.BlockOnSlowStreaming`.
- [I] A patente CLOD com listas de vértices *compartilhadas* entre níveis também é uma otimização de memória/streaming: um só buffer por malha serve todos os LODs, só a tabela muda — reduz o que precisa ser lido do SSD para trocar de LOD. Na Real Engine, o `.rasset` (vértices quantizados, LODs) pode adotar essa ideia.
- [I] Animações retargetadas em tempo real (600 mil animações) é também orçamento de memória: um conjunto de clipes serve corpos diferentes. Equivalente UE5: IK Retargeter em runtime / Retarget Pose from Mesh.

### Gaps
- Nenhuma fonte (oficial ou DF) sobre velocidade de streaming, uso de Kraken/Oodle, orçamento de memória por categoria, ou pop-in ao dirigir rápido na versão lançada.
- Não encontrei patentes de 2019-2026 da Take-Two/Rockstar sobre streaming de mundo, geração de terreno ou mapa procedural; a página de assignee da Justia retornou 403. Recomendado: busca direta no Google Patents por `assignee:"Take-Two Interactive" after:2019` e `assignee:"Rockstar Games"`.
- Não confirmei se o RDR2/GTA V PC usam Oodle (busca sem resultado).

## 5. Criação de conteúdo procedural/assistida por IA, vagas de emprego, declarações da Rockstar/Take-Two

### Takeaway
As vagas da Rockstar mostram um pipeline de arte procedural em Houdini (HDAs, VEX, Houdini Engine, terreno) misturado a arte feita à mão; a mensagem oficial, porém, é de mundo "hand-crafted" e NPCs feitos à mão, não procedurais. Não há confirmação de IA generativa no conteúdo.

### Cited Findings
- [C, vaga] "Senior Technical Artist: Procedural Art": ferramentas para arte de ambiente em "large open worlds"; pipelines de geração procedural com ferramentas internas e de terceiros, integradas "seamlessly" à arte manual; HDAs avançados para criação procedural — [The Muse (Rockstar)](https://www.themuse.com/jobs/rockstargames/senior-technical-artist-procedural-art-3dce27); [Built In LA](https://www.builtinla.com/job/senior-technical-artist-procedural-art/9039383)
- [C, vaga] "Principal Technical Artist – Houdini Terrain Pipeline Specialist" (vaga encerrada) — [Hitmarker](https://hitmarker.net/jobs/rockstar-games-principal-technical-artist-houdini-terrain-pipeline-specialist-357917)
- [C, vaga] Vagas pedem Houdini + VEX e Houdini Engine; uma vaga de Technical Artist datada de 09/09/2026 — [Hitmarker (Tech Artist)](https://hitmarker.net/jobs/rockstar-games-technical-artist-760998); [Greenhouse (Rockstar)](https://job-boards.greenhouse.io/rockstargames/jobs/7737044003)
- [E] Relatório de terceiros argumenta que o Houdini é central no pipeline do GTA VI, mas admite que a Rockstar não publica postmortems — [rockstar-files](https://rockstar-files.pages.dev/reports/04_technical/0327_Houdini_Use.html)
- [C] Garbut: "This world is a cohesive whole with the density of content and a hand-crafted quality to every bit" — [TheGamer](https://www.thegamer.com/gta-6-map-so-massive-story-doesnt-cover-it-all-game-informer-interview/)
- [C/A] NPCs feitos à mão, não gerados proceduralmente — [Wikipedia](https://en.wikipedia.org/wiki/Grand_Theft_Auto_VI)
- [C, patente] Predição de pose com mocap + IA e blocos de animação data-driven (descrição de terceiros, sem número) — [Wccftech](https://wccftech.com/grand-theft-auto-vi-patents-revolutionize-open-world-forever/)

### Inferences
- [I] Padrão provável: Houdini gera terreno base, dispersão de vegetação, props de rua, estradas e variações de prédio (como em Ghost of Tsushima/Horizon), e artistas refinam à mão — "procedural como ponto de partida, mão no acabamento". Equivalente UE5: **PCG Framework** (+ Houdini Engine para UE). A Real Engine já tem PCG Graph (`pcg.md`) e mundos procedurais (`procedural.md`); o recado do GTA VI é a integração não-destrutiva com edição manual (o artista pode sobrescrever a saída procedural sem perder no re-gerar).

### Gaps
- Nenhuma declaração oficial sobre ferramentas internas (editor do RAGE, pipeline de mundo) nem sobre IA generativa.
- Vagas não citam o GTA VI pelo nome; ligação ao jogo é inferência.

## 6. Fatos confirmados após o lançamento (nov/2026)

### Takeaway
Lançado em 19/11/2026; a DF chamou de "the single most visually striking game of the generation" com desempenho "acceptable yet a little disappointing": 30 fps em todos os consoles, 1440p interno, RTGI + reflexos RT em todos, sombras RT só no PS5 Pro, limitado pela CPU.

### Cited Findings
- [A] PS5 Pro segura 30 fps "near-flawless" com frame pacing perfeito (50-75 h de teste, conforme a fonte), queda breve a 24 fps em explosões em massa — [TweakTown](https://www.tweaktown.com/news/114763/gta-6-deemed-the-single-most-visually-striking-game-of-the-generation-by-tech-experts/index.html); [GTA BOOM](https://www.gtaboom.com/digital-foundry-finds-that-gta-6-indeed-runs-best-on-ps5-pro-b72f) (as duas fontes divergem nas horas: 50+ vs ~75)
- [A] Series S mantém RTGI, reflexos RT e cabelo por fios (strand hair), a 720p interno — [GTA BOOM](https://www.gtaboom.com/digital-foundry-finds-that-gta-6-indeed-runs-best-on-ps5-pro-b72f)
- [C] Rockstar confirmou 30 fps nos consoles (ago/2026) — [Wikipedia](https://en.wikipedia.org/wiki/Grand_Theft_Auto_VI)
- [A] Cabelo por fios novo (pesado, precisa de otimização dinâmica), suor na pele, cabelo e roupa animados separadamente — [libertycity (DF trailer 2)](https://libertycity.net/news/gta-6/8997-digital-foundry-analyzes-gta-6s-second.html)
- [A] Upscaler QSSR/PSSR custaria ~4 ms de 33 ms segundo a DF (análise do trailer de lançamento) — [GTA Intel](https://gtaintel.com/news/gta-6-launch-trailer-digital-foundry-analysis)
- [C] Metacritic 99; recordes de vendas (32,5 M em pré-venda) — [Wikipedia](https://en.wikipedia.org/wiki/Grand_Theft_Auto_VI) (números não conferidos em fonte primária)

### Inferences
- [I] Para a Real Engine (meta 200 fps a 1440p): o GTA VI prova que RTGI + reflexo RT + strand hair cabem no Series S a 720p/30, mas ao custo de 33 ms. A lição útil não é o orçamento, é a *escala por plataforma sem tirar o RTGI* (RT em água pequena, SSR no oceano, sombra RT só no hardware que aguenta) — compatível com os presets Baixo..Épico da engine.

### Gaps
- O texto/vídeo original da DF (seções sobre LOD, streaming, folhagem e água) não foi acessado; tudo é secundário.
- Sem análise de PC (não lançado).
