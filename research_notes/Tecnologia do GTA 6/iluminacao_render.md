# GTA 6: iluminação e tecnologia de render (RAGE), com equivalentes na UE5

Notas de pesquisa de 23/11/2026. Legenda de confiança: **[CONFIRMADO]** = Rockstar/registro oficial; **[ANÁLISE]** = Digital Foundry (DF) e imprensa técnica, quase sempre por resumo secundário do vídeo (o vídeo original não foi aberto); **[ESPECULAÇÃO]** = rumor, fórum ou inferência minha.

## 1. O jogo saiu? Linha do tempo e plataformas

### Takeaway
O GTA 6 saiu em 19/11/2026 no PS5 e no Xbox Series X|S (o PC fica para depois). Todas as versões miram 30 fps, sem modo de 60 fps. As análises da DF no lançamento confirmam RTGI e reflexos por raio em todos os consoles; as sombras por raio são exclusivas do PS5 Pro.

### Cited Findings
- [CONFIRMADO] Primeiro adiamento (mai/2025): de "fall 2025" para 26/05/2026 — [TechRadar](https://www.techradar.com/gaming/grand-theft-auto-6-release-date-confirmed-but-its-been-delayed-heres-why); [Kotaku](https://kotaku.com/gta6-grand-theft-auto-6-release-date-delay-may-2026-1851778799)
- [CONFIRMADO] Segundo adiamento (nov/2025) para 19/11/2026, anunciado junto do balanço trimestral da Take-Two — [Push Square](https://www.pushsquare.com/news/2025/11/gta-6-delayed-again-pushed-back-to-november-2026); [GamesRadar](https://www.gamesradar.com/games/grand-theft-auto/gta-6-delayed-6-more-months-as-rockstar-commits-to-november-2026-and-says-sorry)
- [CONFIRMADO via imprensa] Post de pré-carregamento da Rockstar (cerca de 10/11/2026): 30 fps em PS5, PS5 Pro e Series X|S; o PS5 Pro tem PSSR e "ray-traced shadows only available on PlayStation 5 Pro"; não há modo de 60 nem de 40 fps — [TweakTown](https://www.tweaktown.com/news/114598/rockstar-confirms-gta-6-targets-30-fps-on-ps5-pro-with-pssr-and-ray-traced-shadows/index.html); [GTA Boom](https://www.gtaboom.com/rockstar-confirms-gta-6-ps5-pro-upgrades-ahead-of-november-19-launch-f80c); [playday.one](https://playday.one/2026/11/10/gta-6-confirms-ps5-pro-enhancements-and-xbox-cloud-gaming-availability/)
- [ANÁLISE] As reviews saíram antes do lançamento "at midnight tonight". A KitGuru relata que a DF "confirmed the presence of ray-traced global illumination (RTGI) and ray-traced reflections across all console versions" — [KitGuru](https://www.kitguru.net/gaming/matthew-wilson/gta-6-includes-advanced-ray-tracing-even-on-base-consoles/)
- [ANÁLISE] Tech review da DF (resumo da TweakTown, 18/11/2026): PS5 e PS5 Pro renderizam a 1440p interna e miram 30 fps. No Pro, mais de 50 h a 30 fps quase cravados, com queda pontual a 24 fps em explosões; no PS5 base, até 23 fps no mesmo teste. O limite é a CPU ("limited by the console CPU rather than the GPU"). A DF chama o desempenho de "both acceptable yet a little disappointing" — [TweakTown](https://www.tweaktown.com/news/114763/gta-6-deemed-the-single-most-visually-striking-game-of-the-generation-by-tech-experts/index.html)
- [ANÁLISE] Por plataforma (resumo da GTA Boom, 19/11/2026):
  - PS5 Pro: 1440p + PSSR, "a convincing near-4K image".
  - PS5 e Series X: 1440p com upscale espacial mais suave.
  - Series S: 720p interna com "FSR 4" para cerca de 1080p.
  - RTGI e reflexos por raio em todas as versões, inclusive no Series S; o Series S mantém o cabelo em fios.
  - Series X cai a 20-22 fps nos tiroteios mais pesados.
  
  Fonte: [GTA Boom](https://www.gtaboom.com/digital-foundry-finds-that-gta-6-indeed-runs-best-on-ps5-pro-b72f)
- [ANÁLISE] Segundo outra fonte, PS5 e Series X usam um upscale espacial suave feito pelo "Rockstar's own TAA", e um modo de 40 fps foi descartado por limite de CPU — [Consolyte](https://consolyte.gg/articles/gta-6-graphics-comparison-ps5-vs-ps5-pro); [Techtroduce](https://www.techtroduce.com/gta-6-digital-foundry-ps5-pro-performance-review)

### Inferences
- "FSR 4" no Series S (RDNA 2, sem a aceleração de ML do FSR 4) é suspeito. Pode ser erro do resumo; trate como não verificado.
- Gargalo de CPU a 30 fps e RTGI em todos os consoles (até no Series S) indicam uma RTGI de baixo custo: poucos raios por pixel, meia resolução e um cache/denoiser forte. Não é path tracing.

### Gaps
- O vídeo da DF e o post do Newswire não foram lidos direto (403/404 nas tentativas). Os números vêm de resumos.
- O PC não saiu, então não há lista oficial de opções gráficas do GTA 6 (como a do GTA V Enhanced).

## 2. Iluminação global: RTGI, sondas, bake ou híbrido?

### Takeaway
A GI do GTA 6 é por raio (RTGI difusa) em todos os consoles, e a DF vê isso desde o Trailer 2. É muito provável que seja a mesma família da RTGI levada ao GTA V Enhanced de PC (mar/2025). As técnicas antigas do RAGE (patente de "bounce map" e Ambient Mask Volumes, AO em textura 3D por volume) devem continuar como fallback e camada de AO de baixa frequência. Na UE5, o equivalente é o **Lumen** com Hardware Ray Tracing (Final Gather + Surface Cache), não o MegaLights.

### Cited Findings
- [ANÁLISE] Trailer 2 (mai/2025): Alex Battaglia (DF) disse que a RTGI é intrínseca ao jogo ("inherent to the way the game works"). John Linneman apontou bastante luz direta, indireta e natural como sinal de um sistema de RTGI difusa — [LibertyCity](https://libertycity.net/news/gta-6/8997-digital-foundry-analyzes-gta-6s-second.html); [Pure Xbox](https://www.purexbox.com/news/2025/05/gta-6-tech-is-seriously-impressive-but-its-looking-like-a-30fps-game-on-console)
- [CONFIRMADO] Rockstar: todas as cenas do Trailer 2 foram capturadas num PS5 (não Pro) — [Notebookcheck](https://www.notebookcheck.net/PS5-was-behind-every-frame-of-GTA-6-Trailer-2-confirms-Rockstar.1012400.0.html); [VideoGamer](https://www.videogamer.com/features/jaw-dropping-gta-6-trailer-2-captured-on-just-so-ps5-pro-better)
- [ANÁLISE] Demo de cerca de 26-27 min no PS5 base (ago/2026): a DF vê uso extenso de RTGI, com luz que quica e objetos coloridos tingindo as superfícies próximas (color bleeding) e ruído de RT visível em algumas cenas — [MobiDevices](https://mobidevices.com/en/digital-foundry-gta-6); [Vandal](https://vandal.elespanol.com/noticia/2026240028/digital-foundry-analiza-los-graficos-de-gta-6-quiza-lo-mas-impresionante-que-hemos-visto-en-tiempo-real/)
- [ANÁLISE] A RTGI leva em conta a oclusão e a transferência de cor entre interiores, exteriores, personagens e objetos (Vandal, resumo da DF) — [Vandal](https://vandal.elespanol.com/noticia/2026240028/digital-foundry-analiza-los-graficos-de-gta-6-quiza-lo-mas-impresionante-que-hemos-visto-en-tiempo-real/)
- [ANÁLISE] Comparação da DF (set/2026) entre GTA V Enhanced no PC e GTA 6 no PS5: "fairly similar RT implementation" e RTGI parecida. O GTA 6 convence mais pela "higher model and material quality" — [Notebookcheck](https://www.notebookcheck.net/GTA-6-How-does-PS5-compare-to-GTA-5-Enhanced-on-PC.1385330.0.html)
- [CONFIRMADO] O GTA V Enhanced de PC (mar/2025) tem opções de AO, GI, sombras e reflexos por raio. A DF elogiou a RTGI e especulou que a Rockstar a fez para o GTA 6 e a trouxe de volta ao GTA V — [Gamepressure](https://www.gamepressure.com/newsroom/experts-examined-gta-5-enhanced-they-think-rockstar-did-better-th/zf7b3c); [Sportskeeda](https://www.sportskeeda.com/gta/news-gta-6-rtgi-likely-backported-gta-5-enhanced-speculates-analyst); [PC Gamer](https://pcgamer.com/hardware/gta-5-enhanced-performance-analysis-forget-max-settings-my-advice-is-to-enable-very-high-rt-and-enjoy-the-show)
- [CONFIRMADO, depois apagado] O perfil no LinkedIn de Ryan Woods (Principal Engine Programmer) citava a integração de "ray tracing, ray-traced global illumination, procedural generation" no pipeline do motor; o texto foi editado em seguida — [HotHardware](https://hothardware.com/news/gta-6-graphics-details-linkedin); [GamesRadar](https://www.gamesradar.com/games/grand-theft-auto/gta-6-dev-teases-integration-of-cutting-edge-technologies-and-procedural-generation-for-objects-and-environments-in-rockstars-upcoming-open-world); [TweakTown](https://tweaktown.com/news/101886/gta-6-developer-teases-cutting-edge-tech-procedural-generation-for-objects-environments/index.html)
- [CONFIRMADO, patente] US 12,263,404 B2, "Method and apparatus for enhanced graphics rendering in a video game environment":
  - Dados: titular Take-Two Interactive; inventores Hadjadj, Kerr, Reed, Robertson, Shepherd, Alecu e Stirling; prioridade de 02/04/2018 (era do RDR2); concessão em 01/04/2025.
  - Claim 1 (GI): uma textura vista de cima ("bounce map") guarda a luz refletida em RGB por texel; um mapa de profundidade na mesma resolução guarda a altura do terreno; a luz que quica do chão é calculada por pixel e lida dessa textura.
  - Detalhes: box blur, mips e amostragem por interseção cone-plano, com opção de importância por cosseno. A patente diz que dá "many of the benefits of ray tracing without the computational expense".
  - Ambient Mask Volumes: AO numa textura 3D projetada em caixas (prédios) ou no terreno ("1 probe every 2 meters"), aplicada em camadas (interiores e objetos dinâmicos à parte) e assada offline com um cubemap de visibilidade do céu.
  
  Fonte: [Google Patents US12263404B2](https://patents.google.com/patent/US12263404B2/en)
- [ANÁLISE, contexto] O RAGE do RDR2 (2018) já tinha DX12/Vulkan, PBR, nuvens e neblina volumétricas e GI pré-calculada — [GTA Wiki (fandom), baixa confiança](https://gta.fandom.com/wiki/Rockstar_Advanced_Game_Engine)
- [ANÁLISE] Patentes da Take-Two (incl. LOD contínuo US 11978162B2) são ligadas ao GTA 6 pela imprensa, mas "não é certo" que estejam no jogo — [Multiplayer.it](https://multiplayer.it/notizie/gta-6-innovera-mercato-videoludico-10-brevetti-rockstar-games-parlano-nuove-tecnologie.html); [Mein-MMO](https://mein-mmo.de/en/the-characters-in-gta-6-will-really-sweat-11-patents-with-which-rockstar-wants-to-stay-ahead-of-the-competition,1334011/2/)

### Inferences
- [ESPECULAÇÃO] Arquitetura provável: um sistema híbrido. A camada assada/volumétrica (AMV, bounce map do chão, visibilidade do céu) dá a base de baixa frequência e o fallback barato. A RTGI por hardware (raios curtos em meia resolução, com acúmulo temporal e denoiser) cuida da luz indireta dinâmica. O "ruído de RT" que a DF vê é típico de poucos raios por pixel com denoiser temporal.
- Equivalentes na UE5:
  - RTGI: Lumen HWRT (Screen Traces + Surface Cache + radiance cache).
  - AMV: o que mais se parece com a AO de Distance Field ou com o Lumen de baixa qualidade; na Real Engine corresponde ao cache de superfície e às sondas do RealGI (notas-dev/gi.md).
  - Bounce map do chão: não tem equivalente direto na UE5. É uma GI barata e específica para terreno (luz do chão para cima), útil como fallback no preset Baixo.

### Gaps
- Não há palestra GDC/SIGGRAPH da Rockstar sobre a RTGI do GTA 6, nem números de raios por pixel, resolução da GI ou tipo de denoiser.
- Não encontrei confirmação de cache de irradiância, ReSTIR ou sondas dinâmicas.

## 3. Reflexos, sombras, AO, emissivos e muitas luzes à noite

### Takeaway
- **Reflexos:** por raio em superfícies opacas e transparentes (carros, vidro, óculos), com SSR por cima e reflexos planares em espelhos e alguns interiores (escolha estética da Rockstar). Superfícies metálicas ásperas ficam com reflexo ruidoso ou manchado.
- **Sombras:** shadow maps tradicionais (sol e luzes locais, com penumbra que cresce com a distância) nos consoles base; por raio só no PS5 Pro.
- **Neon e muitas luzes:** não há fonte técnica específica.

### Cited Findings
- [ANÁLISE] Trailer 2: reflexos por raio em objetos transparentes (óculos, espelhos, para-brisas) — [LibertyCity](https://libertycity.net/news/gta-6/8997-digital-foundry-analyzes-gta-6s-second.html); [Tech4Gamers](https://tech4gamers.com/gta-6-hybrid-ray-tracing)
- [ANÁLISE] Demo no PS5 (ago/2026):
  - Os reflexos por raio mostram objetos fora da tela, o que o SSR não consegue.
  - Alguns têm resolução interna baixa e borram com a câmera em movimento.
  - Espelhos comuns parecem usar reflexo planar.
  - Na maior parte, sombras do sol e de luzes locais são shadow maps, mais suaves longe do objeto que as projeta; RT pode aparecer em closes.
  
  Fonte: [MobiDevices](https://mobidevices.com/en/digital-foundry-gta-6)
- [ANÁLISE] Reflexos por raio "com soluções híbridas em algumas superfícies para reduzir o custo"; alguns com ruído — [Vandal](https://vandal.elespanol.com/noticia/2026240028/digital-foundry-analiza-los-graficos-de-gta-6-quiza-lo-mas-impresionante-que-hemos-visto-en-tiempo-real/)
- [ANÁLISE] Planar e SSR em algumas áreas são intencionais: "sometimes, those traditional methods really do look better, especially planar reflections in indoor environments". A resolução menor que a do PC causa "noticeable artifacts on occassion" — [Notebookcheck](https://www.notebookcheck.net/GTA-6-How-does-PS5-compare-to-GTA-5-Enhanced-on-PC.1385330.0.html)
- [ANÁLISE, resumo de busca, confiança média] Na review, a Rockstar às vezes põe informação de tela sobre o reflexo por raio (artefatos de SSR), e metais ásperos dão reflexos ruidosos ou manchados — [NeoGAF thread da DF Tech Review, fórum](https://www.neogaf.com/threads/digital-foundry-grand-theft-auto-6-digital-foundry-tech-review-the-game-of-the-generation.1703495/page-3)
- [ANÁLISE] Análise de trailer anterior: água com SSR e materiais brilhantes (metal, plástico) com reflexo por raio — [Sportskeeda, baixa confiança](https://sportskeeda.com/gta/news-bilibili-gta-6-trailer-reportedly-examined-popular-analyst-disclosed-interesting-details)
- [CONFIRMADO + ANÁLISE] Sombras por raio só no PS5 Pro; elas eliminam os "chunky shadow artifacts" do PS5 base — [TweakTown](https://www.tweaktown.com/news/114763/gta-6-deemed-the-single-most-visually-striking-game-of-the-generation-by-tech-experts/index.html); [GTA Boom](https://www.gtaboom.com/digital-foundry-finds-that-gta-6-indeed-runs-best-on-ps5-pro-b72f)
- [ANÁLISE] AO: a DF não cita AO por nome no GTA 6, só a oclusão dentro da GI — [Vandal](https://vandal.elespanol.com/noticia/2026240028/digital-foundry-analiza-los-graficos-de-gta-6-quiza-lo-mas-impresionante-que-hemos-visto-en-tiempo-real/). No GTA V Enhanced há opção de AO por raio — [Gamepressure](https://www.gamepressure.com/newsroom/experts-examined-gta-5-enhanced-they-think-rockstar-did-better-th/zf7b3c)
- [ANÁLISE, review] A TheGamer destaca as áreas de neon de Capri e o visual noturno de Vice City; a Windows Central elogia o ciclo dia e noite com "dynamic lighting" — [TheGamer](https://www.thegamer.com/grand-theft-auto-6-review/); [Windows Central](https://www.windowscentral.com/gaming/xbox/grand-theft-auto-6-xbox-review)

### Inferences
- [ESPECULAÇÃO] Neon e muitas luzes: como nenhuma análise fala em sombra por raio para luzes locais nos consoles base, é provável que os emissivos entrem pela RTGI (luz emissiva captada pelos raios da GI, como o Lumen faz com materiais emissivos) e que as luzes de rua e de vitrine sejam luzes clusterizadas, a maioria sem sombra ou com shadow map em cache. Isso fica longe do MegaLights, que a Rockstar não mostra.
- Equivalentes na UE5:

  | GTA 6 | UE5 |
  |---|---|
  | Reflexos por raio + SSR + planar | Lumen Reflections HWRT (Hit Lighting), SSR e Planar Reflection Actor |
  | Shadow maps (consoles base) | Cascaded Shadow Maps ou VSM; o "penumbra com distância" lembra o PCSS / SMRT do VSM |
  | Sombras por raio (PS5 Pro) | Ray Traced Shadows |
  | AO | Lumen Short Range AO / RTAO |

### Gaps
- Nenhuma fonte técnica sobre quantas luzes com sombra há à noite, se existe amostragem estocástica de luzes (tipo MegaLights/ReSTIR DI) ou se os neons projetam sombra.
- Sem informação sobre contact shadows nem sobre a resolução dos cascades.

## 4. Céu, atmosfera, nuvens, neblina, clima e hora do dia

### Takeaway
As reviews e a DF confirmam nuvens volumétricas procedurais ligadas aos sistemas de clima, chuva que muda a superfície da água, água diferente por região e "volumetric mist". Não há detalhe técnico publicado do GTA 6 em si. O precedente direto é o sistema atmosférico integrado do RDR2 (SIGGRAPH 2019).

### Cited Findings
- [ANÁLISE, resumo de busca, confiança média] O GTA 6 tem nuvens volumétricas procedurais que acompanham os sistemas de clima; a água muda por local (oceano mais agitado) e a chuva afeta a superfície da água — [NeoGAF thread da DF Tech Review](https://www.neogaf.com/threads/digital-foundry-grand-theft-auto-6-digital-foundry-tech-review-the-game-of-the-generation.1703495/page-3)
- [ANÁLISE, review] O ciclo dia e noite recebe "big praise for its weather effects and dynamic lighting", e há "volumetric mist" — [Windows Central](https://www.windowscentral.com/gaming/xbox/grand-theft-auto-6-xbox-review)
- [ANÁLISE] O RAGE do RDR2 já tinha nuvens e neblina volumétricas — [GTA Wiki (baixa confiança)](https://gta.fandom.com/wiki/Rockstar_Advanced_Game_Engine)
- [CONFIRMADO, precedente] Rockstar na SIGGRAPH 2019 (Advances in Real-Time Rendering): "Creating the Atmospheric World of Red Dead Redemption 2: A Complete and Integrated Solution" (Fabian Bauer), sobre céu, nuvens, neblina e volumétricos num sistema só — [Advances in Real-Time Rendering 2019](https://advances.realtimerendering.com/s2019/index.htm)
- [ANÁLISE] Trailer 2: suor nos personagens; cabelo e roupa animados à parte — [Tech4Gamers](https://tech4gamers.com/gta-6-new-bar-for-open-world-visuals)

### Inferences
- [ESPECULAÇÃO] É provável que reaproveite o modelo do RDR2: nuvens em raymarching num domínio de baixa resolução com reprojeção temporal, neblina em froxels, atmosfera com LUTs. Na UE5, o equivalente é Volumetric Clouds + Sky Atmosphere + Exponential Height Fog com Volumetric Fog. A Real Engine já tem os três (notas-dev/luz.md).
- [ESPECULAÇÃO] Superfícies molhadas e poças: nenhuma fonte técnica descreve como são feitas. O padrão da indústria é escurecer o albedo, baixar a rugosidade e usar máscara de poça por altura ou pintura, com o reflexo vindo do sistema RT + SSR.

### Gaps
- Nenhuma fonte confiável sobre furacões ou tempestades extremas no jogo lançado. Não confirmado.
- Nada sobre o modelo de atmosfera (Bruneton/Hillaire), o raymarching das nuvens ou o sombreamento de nuvem no chão.

## 5. Pós-processo, imagem e "fotorrealismo"

### Takeaway
A imagem final vem de 1440p interna com reconstrução: PSSR no Pro e TAA/upscale espacial da própria Rockstar nos outros. Por isso fica mais suave, com cintilação, serrilhado e ruído nos reflexos. Exposição, DOF, motion blur, granulação e lentes não têm análise técnica publicada que eu tenha achado. A DF chama o jogo de, talvez, a coisa mais impressionante já vista em tempo real.

### Cited Findings
- [ANÁLISE] Cerca de 2560×1440 interna, sem resolução dinâmica visível, upscale espacial simples para 4K "no clear sign of FSR 2 or TAAU", com imagem suave, cintilação e serrilhado — [MobiDevices](https://mobidevices.com/en/digital-foundry-gta-6); [Vandal](https://vandal.elespanol.com/noticia/2026240028/digital-foundry-analiza-los-graficos-de-gta-6-quiza-lo-mas-impresionante-que-hemos-visto-en-tiempo-real/)
- [ANÁLISE] Contradição: outra fonte fala em upscale espacial "through Rockstar's own TAA" no PS5 e no Series X — [Consolyte](https://consolyte.gg/articles/gta-6-graphics-comparison-ps5-vs-ps5-pro)
- [ANÁLISE] A DF sobre a demo: "quizá lo más impresionante que hemos visto en tiempo real"; Push Square: "the most impressive example of real-time rendering we've ever seen" — [Vandal](https://vandal.elespanol.com/noticia/2026240028/digital-foundry-analiza-los-graficos-de-gta-6-quiza-lo-mas-impresionante-que-hemos-visto-en-tiempo-real/); [Push Square](https://www.pushsquare.com/news/2026/08/tech-experts-call-gta-6-the-most-impressive-example-of-real-time-rendering-weve-ever-seen)
- [ANÁLISE] Na review: "the single most visually striking game of the generation" — [TweakTown](https://www.tweaktown.com/news/114763/gta-6-deemed-the-single-most-visually-striking-game-of-the-generation-by-tech-experts/index.html)
- [ANÁLISE] Trailer 1 (dez/2023): a DF tratou de luz e reflexos por raio, cabelo, sombras e 60 fps — [Pure Xbox](https://purexbox.com/news/2023/12/digital-foundry-breaks-down-grand-theft-auto-6-trailer-1)

### Inferences
- Equivalentes na UE5:
  - Reconstrução: TSR, ou DLSS/FSR/PSSR. A Real Engine já tem TSR e os SDKs (notas-dev/upscaling.md).
  - Cintilação e ruído: o mesmo problema que o Lumen tem com poucos raios. Mostra que a Rockstar escolheu GI dinâmica por raio em vez de nitidez.

### Gaps
- Sem análise de eye adaptation, DOF (bokeh), motion blur por objeto, granulação, aberração ou color grading/LUT no jogo lançado. As fontes acessíveis não cobrem esses pontos.

## 6. Vagas, patentes e falas de desenvolvedores

### Takeaway
As vagas da Rockstar pedem engenharia de render clássica (luz, sombra, shader, culling, cena) em C/C++, HLSL, D3D11+/Vulkan, e pelo menos uma pede APIs de ray tracing em tempo real (DXR). A patente principal de GI é de 2018, da era do RDR2, e descreve uma GI barata "sem o custo do ray tracing".

### Cited Findings
- [CONFIRMADO] Vaga de Graphics Programmer (Carlsbad, CA): "rendering, lighting, shadows, shaders, culling, scene management"; pede C/C++, HLSL, Direct3D 11+/OpenGL/Vulkan, RenderDoc/PIX — [Greenhouse Rockstar 6248752003](https://job-boards.greenhouse.io/rockstargames/jobs/6248752003)
- [CONFIRMADO] Outra vaga pede "experience using modern real time ray tracing APIs (DXR, NVX or vendor specific)" — [Greenhouse Rockstar 6250337003](https://job-boards.greenhouse.io/rockstargames/jobs/6250337003)
- [CONFIRMADO] Vaga em Andover, MA: "cutting-edge graphics and visual effect systems" — [Greenhouse Rockstar 6250336003](https://job-boards.greenhouse.io/rockstargames/jobs/6250336003)
- [CONFIRMADO] Patente US12263404B2 (bounce map + Ambient Mask Volumes), detalhada na seção 2 — [Google Patents](https://patents.google.com/patent/US12263404B2/en)
- [CONFIRMADO, editado] LinkedIn de Ryan Woods: RT, RTGI e geração procedural no motor — [HotHardware](https://hothardware.com/news/gta-6-graphics-details-linkedin)

### Inferences
- A patente de 2018 com AO em textura 3D e bounce do chão ajuda a explicar como o GTA 6 tem fallback e camada de baixa frequência mesmo com RTGI. Também é uma boa ideia barata para o preset Baixo da Real Engine (GPU sem RT).

### Gaps
- Os textos completos das vagas não foram lidos (só trechos); podem ter sido fechadas.
- Nenhuma patente específica de clima ou nuvens do GTA 6 foi encontrada nesta pesquisa.
