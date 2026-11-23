# Veículos e trânsito no GTA 6 (RAGE): renderização, simulação, IA de tráfego e equivalentes na UE5

Pesquisa feita em 23/11/2026. Contexto de data: o lançamento estava marcado para **19/11/2026** (PS5 e Xbox Series; PC
depois) ([TechRadar](https://www.techradar.com/gaming/grand-theft-auto-6-delayed-again-but-itll-still-ship-in-2026);
[PCGamesN](https://www.pcgamesn.com/gta-6-release-date)). O embargo das análises caiu em **18/11/2026** (Forbes, IGN,
Guardian, Eurogamer, GamesRadar, TheSixthAxis e a análise técnica da Digital Foundry)
([Traxion](https://traxion.gg/grand-theft-auto-6-reviews-praise-series-best-car-handling/);
[Forbes](https://www.forbes.com/sites/paultassi/2026/11/18/grand-theft-auto-6-review-a-perfect-crime/);
[VGTimes sobre a DF](https://vgtimes.com/gaming-news/175004-ray-tracing-realistic-physics-and-30-fps-digital-foundry-takes-a-technical-deep-dive-into-gta-6.html)).
Não encontrei nenhuma reportagem dizendo que o lançamento foi adiado de novo. Então há fatos das análises, mas ainda
quase nenhuma engenharia reversa ou documentação técnica pós-lançamento.

Legenda de confiança: **[CONFIRMADO]** = Rockstar ou análises do jogo final; **[ESPECIALISTA]** = Digital Foundry ou
imprensa técnica; **[TRAILER]** = observado em trailer ou screenshot oficial (pode diferir do jogo); **[ESPECULAÇÃO]** =
inferência minha ou de fãs; **[BAIXA]** = vazamento, fã, agregador ou site de conteúdo duvidoso.

Limitação importante: o site da Digital Foundry (digitalfoundry.net) e o da VGC recusaram acesso automatizado (403 ou
bloqueio). Os dados da DF vêm de resumos secundários (VGTimes, GTABoom, Techtroduce, ResetEra, Traxion). Confira no vídeo
ou no artigo original antes de citar números exatos.

## 1. Renderização de veículos (pintura, reflexos na lataria e no vidro, faróis e lanternas à noite, sujeira e chuva, interiores)

### Takeaway
Nenhuma fonte acessível descreve o shader de pintura do GTA 6 (clearcoat ou flocos). O que está confirmado é o pacote de
iluminação: RTGI e reflexos por ray tracing em todos os consoles, e sombras por RT só no PS5 Pro. Isso alimenta os
reflexos de lataria, vidro e cromo e as poças à noite. Os trailers mostram detalhes de faróis, como retrorrefletores e
tachões que acendem com o farol, ruas molhadas espelhando carros e neon, e interiores detalhados.

### Cited Findings
- [ESPECIALISTA] A DF diz que GTA 6 usa RTGI e reflexos por ray tracing no PS5 e no PS5 Pro, com sombras por RT
  exclusivas do PS5 Pro. Chamou o RT de "arguably the best" que já viu. Os dois consoles renderizam em 1440p de base; o
  Pro reconstrói para 4K com PSSR. A meta é 30 fps — [VGTimes](https://vgtimes.com/gaming-news/175004-ray-tracing-realistic-physics-and-30-fps-digital-foundry-takes-a-technical-deep-dive-into-gta-6.html);
  [GTABoom](https://www.gtaboom.com/digital-foundry-finds-that-gta-6-indeed-runs-best-on-ps5-pro-b72f);
  [TechPowerUp](https://www.techpowerup.com/355004/gta-6-reportedly-plays-best-on-ps5-pro-xbox-series-s-struggles).
- [ESPECIALISTA] Segundo a DF, os postes de luz continuam visíveis e funcionando a longa distância. O céu é fisicamente
  baseado, com nuvens volumétricas e clima dinâmico por distrito. O HDR tem realces especulares acima de 1.500 nits —
  [VGTimes](https://vgtimes.com/gaming-news/175004-ray-tracing-realistic-physics-and-30-fps-digital-foundry-takes-a-technical-deep-dive-into-gta-6.html).
- [ESPECIALISTA] No trailer 2 (maio/2025), a DF elogiou os reflexos em vidro, como para-brisas, garrafas e janelas de
  prédios — [Liberty City (resumo da DF)](https://libertycity.net/news/gta-6/8997-digital-foundry-analyzes-gta-6s-second.html);
  [Purexbox](https://www.purexbox.com/news/2025/05/gta-6-tech-is-seriously-impressive-but-its-looking-like-a-30fps-game-on-console).
- [ESPECIALISTA] A Notebookcheck comparou com o GTA 5 Enhanced e disse que os carros são reflexivos, "mas não tanto".
  Ela especula que seria pior para o trânsito se todo carro tivesse acabamento de espelho. A leitura implícita é que há
  variação de rugosidade e desgaste da pintura por veículo — [Notebookcheck](https://www.notebookcheck.net/GTA-6-How-does-PS5-compare-to-GTA-5-Enhanced-on-PC.1385330.0.html).
- [TRAILER] No trailer 2, há reflexos por RT no "headlight pod" cromado de uma moto. Também aparece uma pintura de dois
  tons, laranja queimado e branco desbotado, em uma picape velha, o que indica pintura desgastada ou oxidada como material
  próprio — [The Drive](https://www.thedrive.com/news/new-gta-6-trailer-is-here-and-vice-citys-cars-have-never-looked-better).
- [TRAILER] Os retrovisores dos carros mostram reflexos reais da cena — [PC Gamer](https://pcgamer.com/the-gta-6-trailer-is-packed-with-little-details-here-are-some-of-the-most-promising-and-exciting).
- [TRAILER] Os tachões refletivos ("cat's eyes") da faixa central acendem quando os faróis passam. Os coletes refletivos
  de garis também acendem sob o farol (1:27 do trailer 2) — [DBLTAP](https://www.dbltap.com/features/gta-6-trailer-secrets-tech).
- [TRAILER] A rua molhada à noite em Vice City tem poças que espelham carros e o neon do Ocean View Hotel. O Kotaku
  achou os reflexos de meia distância "bagunçados", com cara de configuração de console, e viu dithering nos reflexos
  distantes — [Kotaku](https://kotaku.com/damn-this-gta-6-screenshot-looks-so-good-people-dont-believe-its-real-2000742750).
- [TRAILER] As barras de luz das viaturas têm destaque forte na cena da batida policial — [The Drive](https://www.thedrive.com/news/new-gta-6-trailer-is-here-and-vice-citys-cars-have-never-looked-better).
- [TRAILER] Uma imagem oficial mostra o interior detalhado de um Bravado Gauntlet Hellfire — [The Drive](https://www.thedrive.com/news/new-gta-6-trailer-is-here-and-vice-citys-cars-have-never-looked-better).
- [TRAILER] Carros atravessando o pântano deformam a lama de forma realista. A comparação feita é com MudRunner — [DBLTAP](https://www.dbltap.com/features/gta-6-trailer-secrets-tech).
  [ESPECIALISTA] Nas análises do jogo final, a DF fala em deformação do terreno nas corridas off-road e em "heat haze"
  sobre o asfalto quente — [Traxion](https://traxion.gg/grand-theft-auto-6-reviews-praise-series-best-car-handling/);
  [VGTimes](https://vgtimes.com/gaming-news/175004-ray-tracing-realistic-physics-and-30-fps-digital-foundry-takes-a-technical-deep-dive-into-gta-6.html).
- [BAIXA] Uma wiki de fã diz que, à noite, neon, faróis e lanternas interagem em tempo real com superfícies molhadas, e
  atribui à DF a frase "iluminação totalmente por ray tracing". Não consegui verificar — [The Games Wiki](https://thegameswiki.com/gta6/wiki/weather-and-environment).

### Inferences
- [ESPECULAÇÃO] Como só o Pro tem sombras por RT, os faróis e as lanternas dos carros da IA provavelmente são luzes
  locais sem sombra ou com sombra por mapa só no carro do jogador. Isso também é o padrão do GTA 5 e do RDR2. Os carros
  distantes devem virar sprites ou cartões emissivos para os faróis continuarem visíveis até o horizonte. A DF cita
  "veículos em movimento visíveis até o horizonte" ([VGTimes](https://vgtimes.com/gaming-news/175004-ray-tracing-realistic-physics-and-30-fps-digital-foundry-takes-a-technical-deep-dive-into-gta-6.html)).
- [ESPECULAÇÃO] A pintura provavelmente usa um shader de carro dedicado com duas camadas especulares (base metálica com
  flocos e um clearcoat), padrão desde o GTA 4/5. O reflexo vem dos reflexos por RT em vez de cubemaps. A sujeira e a
  chuva entram como máscaras que mudam rugosidade e albedo. A Notebookcheck viu variação de brilho entre carros, o que
  sugere desgaste por instância.
- Equivalente na UE5: o **Clear Coat** Shading Model do Substrate/Legacy (Clear Coat, Clear Coat Roughness, uma segunda
  normal), os flocos por textura de normal em tile ou o "Car Paint" do Substrate, e reflexos pelo **Lumen** ou por Hardware
  RT Reflections. Os faróis distantes ficam com **Light Function / emissivos + Lumen** e os retrorrefletores com materiais
  emissivos dependentes da direção da luz (não há isso pronto na UE5).

### Gaps
- Nenhuma fonte acessível descreve o shader de pintura (flocos, clearcoat, iridescência), a quantidade de luzes de farol
  com sombra, os faróis volumétricos ou o modelo de sujeira e chuva nos carros do jogo final. A análise técnica da DF deve
  ter esses detalhes, mas o site bloqueou o acesso.
- Não confirmei se os interiores de todos os carros da IA são completos ou se há LOD de interior (por exemplo, janelas
  com interior falso).

## 2. Física e dirigibilidade, suspensão, dano e deformação, destruição, barcos, aeronaves e motos

### Takeaway
As análises de 18/11/2026 dizem que a dirigibilidade é a melhor da série: peso e suspensão de verdade, carros que deslizam,
um meio-termo entre o peso do GTA 4 e a resposta do GTA 5. A DF compara com Gran Turismo e Forza. Há deformação de
painéis que **afeta a dirigibilidade**, mas o modelo exato (soft-body, deformação por vértice ou danos pré-autorados) não
foi divulgado. As motos são o ponto fraco ("upright and stiff"). Não achei detalhes de barcos e aeronaves nas análises.

### Cited Findings
- [CONFIRMADO / análise] IGN (10/10): a dirigibilidade é "the strongest it's ever been in the series". Os carros podem
  deslizar de verdade e têm o peso e a suspensão que faltavam no GTA 5. As motos foram criticadas como "upright and stiff" — [Traxion](https://traxion.gg/grand-theft-auto-6-reviews-praise-series-best-car-handling/).
- [ESPECIALISTA] DF (John Linneman): "The racing and car simulation here is in many ways on par with dedicated games like
  Gran Turismo or Forza". O equilíbrio fica entre o peso do GTA 4 e a resposta do GTA 5, com "tactile vehicle physics" e
  deformação de superfície nas pistas de terra (Thrillbilly Mud Club). O jogo tem ligas de racha, demolition derby e
  circuitos de asfalto para veículos de duas e quatro rodas — [Traxion](https://traxion.gg/grand-theft-auto-6-reviews-praise-series-best-car-handling/).
- [ESPECIALISTA] Segundo resumo da análise da DF, os veículos têm deformação de painéis da carroceria que afeta
  diretamente a dirigibilidade — [VGTimes](https://vgtimes.com/gaming-news/175004-ray-tracing-realistic-physics-and-30-fps-digital-foundry-takes-a-technical-deep-dive-into-gta-6.html).
- [CONFIRMADO / análise] The Guardian (5/5): os controles são complexos e às vezes brigam com a intenção do jogador. O
  carro capotou ao bater numa guia. Nos rachas, os rivais se recuperam rápido das batidas ("occasionally unfair"), o que
  sugere ajuda ou rubber-banding na IA de corrida — [Traxion](https://traxion.gg/grand-theft-auto-6-reviews-praise-series-best-car-handling/).
- [CONFIRMADO / análise] Forbes: a direção está "totally fine", há muito menos atropelamento acidental de pedestres e há
  câmeras de batida ("crash cams") — [Forbes](https://www.forbes.com/sites/paultassi/2026/11/18/grand-theft-auto-6-review-a-perfect-crime/)
  (via resumo da busca; a página recusou o acesso direto).
- [CONFIRMADO / análise] Há combustível que acaba, muitas modificações que mudam aparência, estatísticas e
  dirigibilidade, ruas mais estreitas e chaves keyless e rastreadores nos carros (app WAiNK). Um carro destruído continua
  sendo seu se houver vaga na garagem — [Traxion (GamesRadar, TheSixthAxis, Eurogamer)](https://traxion.gg/grand-theft-auto-6-reviews-praise-series-best-car-handling/).
- [TRAILER] Há um amassado na porta traseira de um Declasse Tulip no trailer 1. Pneus baleados derrubam o carro sobre o
  aro — [Sportskeeda](https://www.sportskeeda.com/gta/news-first-look-gta-6-car-deformation-physics-seemingly-found-trailer);
  [timesaver.gg (BAIXA)](https://timesaver.gg/blog/gta-6-car-damage-driving-physics-confirmed-2026).
- [BAIXA] GTA Intel (pré-lançamento): a Rockstar não detalhou deformação, destruição de prédios nem ragdoll, e o modelo
  de deformação era desconhecido — [GTA Intel](https://gtaintel.com/news/gta-6-physics-and-destruction). Páginas de
  conteúdo falam em "soft-body multi-camada" e para-choques destacáveis sem nenhuma fonte; trate como não confirmado — [gamecryst](https://gamecryst.com/gta-6/news/gta-6-driving-vehicles-extended-look/);
  [Yahoo Tech](https://tech.yahoo.com/gaming/articles/gta-6-leaning-advanced-physics-004339098.html).
- [BAIXA] Uma análise especulativa não oficial (maio/2026) aposta que o GTA 6 mantém a deformação pré-autorada e com
  script da Rockstar, sem soft-body completo — [rockstar-files](https://rockstar-files.pages.dev/reports/02_development/0199_Vehicle_Damage_Models.html).
- [TRAILER] A corrente de Jason reage a cada movimento na moto (física secundária). Um jet-ski aparece rebocado por uma
  Bison — [DBLTAP](https://www.dbltap.com/features/gta-6-trailer-secrets-tech);
  [The Drive](https://www.thedrive.com/news/new-gta-6-trailer-is-here-and-vice-citys-cars-have-never-looked-better).
- Contexto histórico [BAIXA a média]: o GTA 4 tinha a deformação mais realista da série; o GTA 5 reduziu isso a padrões
  de dano predefinidos e cortou o amassado do teto, provavelmente por causa da câmera em primeira pessoa — [Sportskeeda](https://www.sportskeeda.com/gta/news-first-look-gta-6-car-deformation-physics-seemingly-found-trailer);
  [post de fã](https://aaronshenny.github.io/gta6-news/posts/2026-02-16-gta-vi-s-vehicle-damage-will-roof-deformation-return-after-gta-v-s-first-person-compromise).
  No GTA 5, a deformação é ajustada por dados em `handling.meta`/`vehicles.meta` (multiplicador de deformação) e tem
  limites de entortar roda e de afundar o interior. O soft-body vem de mods por script, não do jogo base — [gta5-mods: Better damage](https://ms.gta5-mods.com/vehicles/better-damage-for-vanilla-cars-manual-install);
  [LowerDamageLimits](https://www.gta5-mods.com/scripts/lowerdamagelimits);
  [Next Generation Car Damage](https://www.gta5-mods.com/scripts/next-generation-car-damage).
- [CONFIRMADO, vagas] As vagas de física da Rockstar descrevem a equipe: em Londres/Edimburgo, "vehicle dynamics, water,
  destructible objects and rope"; em Carlsbad, a vaga é para manter e expandir os sistemas de veículos existentes. Ou
  seja, veículos, água e destruição são sistemas próprios da RAGE, não middleware — [Greenhouse (Rockstar)](https://job-boards.greenhouse.io/rockstargames/jobs/6578255003)
  (a página estava fechada quando abri; o texto veio do resumo da busca);
  [Hitmarker](https://hitmarker.net/jobs/rockstar-games-senior-physics-programmer-876444).

### Inferences
- [ESPECULAÇÃO] A frase "deformação de painéis que afeta a dirigibilidade" combina com o modelo herdado do GTA 4/5:
  deslocamento por vértice da malha da carroceria a partir de um campo de dano local, guardado por veículo como textura
  ou grade de deslocamentos e aplicado no vertex shader, com um custo físico em alinhamento, rodas e motor. Um soft-body
  completo seria caro com centenas de carros. O mais provável é deformação procedural por vértice, limitada por dados,
  mais peças destacáveis (para-choque, capô, portas, vidros) como corpos rígidos separados. A confiança é média: não há
  fonte técnica.
- [ESPECULAÇÃO] A suspensão "de verdade" e o deslizamento sugerem um modelo de pneu mais rico (curvas de escorregamento
  longitudinal e lateral) e transferência de carga melhor. As pistas de terra e lama deformáveis indicam um heightfield
  dinâmico só nas zonas off-road (como o RDR2, que já deformava lama e neve).
- Equivalente na UE5: **Chaos Vehicles** (Chaos Wheeled Vehicle Movement Component: motor, câmbio e diferencial por
  curvas; rodas como Blueprints de dados com atrito, suspensão e freio). A deformação na City Sample é um **Control Rig
  compartilhado (`CR_Frame_Destructible`) com plasticidade de constraint**, que deforma de forma permanente depois de um
  limiar — [Epic, City Sample](https://dev.epicgames.com/documentation/unreal-engine/city-sample-project-unreal-engine-demonstration).
  A destruição de peças fica com o **Chaos Destruction** (Geometry Collections). Barcos e aviões não têm equivalente
  pronto na UE5 além do Water plugin (Buoyancy) e da física genérica.

### Gaps
- Não achei detalhes de barcos (hidrodinâmica, ondas), aviões ou helicópteros em nenhuma análise acessível.
- O modelo de deformação (por vértice, soft-body ou autorado), o número de peças destacáveis e se o teto amassa de novo
  em primeira pessoa continuam sem confirmação técnica.
- A frequência do modelo de física (Hz), os sub-passos e se a IA usa física completa ou simplificada também não foram
  encontrados.

## 3. Densidade e IA do trânsito, carros estacionados, pedestres e trânsito, LOD dos carros distantes, simulados × renderizados

### Takeaway
O jogo final tem trânsito "realisticamente denso" (IGN) em ruas mais estreitas. A DF diz que veículos em movimento e
pedestres ficam visíveis até o horizonte, com trocas de LOD "virtually imperceptible". Também diz que os pedestres
reagem de forma autônoma, por exemplo procurando abrigo num tiroteio. Nenhuma fonte dá números de carros simulados ou
renderizados. A melhor pista da arquitetura é a patente de navegação virtual da Rockstar (ver seção 4).

### Cited Findings
- [CONFIRMADO / análise] IGN: o trânsito é "realistically dense". IGN e GamesRadar: as ruas são mais estreitas e
  realistas, e "countless more variables" afetam a direção — [Traxion](https://traxion.gg/grand-theft-auto-6-reviews-praise-series-best-car-handling/).
- [ESPECIALISTA] DF: veículos em movimento e multidões continuam visíveis até o horizonte. As transições de LOD são
  "virtually imperceptible" mesmo longe. Os pedestres agem de forma autônoma, reagem a eventos e procuram abrigo em
  tiroteios — [VGTimes](https://vgtimes.com/gaming-news/175004-ray-tracing-realistic-physics-and-30-fps-digital-foundry-takes-a-technical-deep-dive-into-gta-6.html).
- [ESPECIALISTA] O PS5 base tem pequenas quedas de quadro na "direção intensa", o que indica que o streaming e a
  população em alta velocidade são um ponto de carga de CPU — [ResetEra (trecho do artigo da DF)](https://www.resetera.com/threads/grand-theft-auto-6-digital-foundry-tech-review-the-game-of-the-generation.1690775/).
- [CONFIRMADO / análise] Forbes: há muito menos atropelamento acidental de pedestres que nos GTAs anteriores — [Forbes](https://www.forbes.com/sites/paultassi/2026/11/18/grand-theft-auto-6-review-a-perfect-crime/).
- [TRAILER] Nos trailers há filas de carros parados em cruzamentos (Banshee, Comet, Schafter) e as vias são dominadas
  por picapes. Isso mostra composição do trânsito por região, como "Florida = picapes". Um trem urbano passa por cima da
  rua — [The Drive](https://www.thedrive.com/news/new-gta-6-trailer-is-here-and-vice-citys-cars-have-never-looked-better).
- [CONFIRMADO, vaga] A vaga de "Vehicle AI/Gameplay Programmer" (Edimburgo) cobre comportamento de veículos e sistemas
  de **população** em colaboração com as equipes de física, rede, design e engine — [The Muse (resumo da busca; a página recusou o acesso)](https://themuse.com/jobs/rockstargames/vehicle-aigameplay-programmer).
- Referência UE5 ([Epic, City Sample](https://dev.epicgames.com/documentation/unreal-engine/city-sample-project-unreal-engine-demonstration)):
  - O trânsito é criado por um Mass Spawner ao longo dos pontos do **ZoneGraph** (corredores leves com tags). Ele não usa
    StateTree, diferente da multidão, que usa.
  - Os carros estacionados têm um spawner próprio, a partir de uma nuvem de pontos procedural ao longo das ruas. Os
    cruzamentos têm um spawner separado.
  - Há sliders de densidade (Crowd, Traffic, Parked Cars) e o áudio dos carros da IA é feito por MetaSounds pela velocidade.
  - Pedestres perto são MetaHumans completos; os longe são Static Meshes com animação por vértice.
- Referência UE5, números da Matrix Awakens: 45.073 carros estacionados (38.146 dirigíveis), 35.000 pedestres
  MetaHuman simulados, 7.000 prédios e 27.848 postes — [Xbox Wire](https://news.xbox.com/en-us/2021/12/09/matrix-awakens-unreal-engine-5-experience/);
  [PC Gamer](https://pcgamer.com/uk/the-matrix-awakens-contains-38146-driveable-cars-of-which-we-can-drive-zero);
  [Dark Horizons](https://www.darkhorizons.com/?p=151475). Uma reportagem cita 17.000 veículos de trânsito simulados e
  destrutíveis; não vi esse número na fonte primária da Epic — [AOL/Variety](https://www.aol.com/news/inside-matrix-awakens-epic-games-040021751.html).
- [ESPECIALISTA, UE5] O MassTraffic é descrito como um conjunto muito complexo de regras e gerenciadores só para os
  veículos (troca de faixa, semáforos, estacionados). Há comando para desligar o multithreading (`ai.traffic.parallelize 0`)
  e asserts conhecidos na 5.3/5.4 — [Fórum Epic](https://forums.unrealengine.com/t/mass-traffic-assertion-failed-zone-graph-storage/2244928).

### Inferences
- [ESPECULAÇÃO] A arquitetura provável segue a RAGE do GTA 5 e a patente: uma rede de nós e links de estrada, com
  densidade definida por tipo de via e região. Haveria um anel de simulação completa perto do jogador (física e IA
  completas), um anel intermediário de "dummies" que seguem a spline da via sem física de rodas, e carros distantes como
  instâncias leves ou impostores com faróis emissivos. Isso explicaria ver carros até o horizonte com LOD imperceptível.
  O número exato não foi divulgado. No GTA 5 o orçamento era de dezenas de carros ambientes; no GTA 6, "realistically
  dense" e congestionamentos nos trailers sugerem centenas simultâneos, com a maioria em LOD barato.
- [ESPECULAÇÃO] Os congestionamentos sugerem que a IA segura a fila de verdade (seguir o carro da frente, semáforos,
  gap acceptance) em vez de apagar carros parados. Isso combina com o redirecionamento por via congestionada descrito na
  patente.
- Equivalente na UE5: **Mass Entity + MassTraffic + ZoneGraph**. Os carros perto são trocados por Chaos Vehicles com
  física (o "Physics Vehicle Template Actor"); os longe são Instanced Static Mesh pelo Mass Representation (LOD de
  representação). A densidade é configurada por spawners. A UE5 não traz roteamento por congestionamento nem
  personalidade por motorista prontos: isso precisa ser escrito.

### Gaps
- Não há números oficiais nem medidos de carros simulados, renderizados ou estacionados, nem raios de LOD de
  população, no GTA 6.
- Não achei fonte sobre semáforos, gap acceptance, reação dos motoristas a tiros e perseguições no jogo final, nem
  sobre pedestres atravessando a rua (faixas) e o "jaywalking".

## 4. Patentes da Rockstar / Take-Two (2019-2026) sobre veículos, trânsito, IA de direção e dano

### Takeaway
A patente relevante achada é **"System and Method for Virtual Navigation in a Gaming Environment"** (Take-Two; inventores
David Hynd, diretor associado de tecnologia, e Simon Parr, programador líder de IA da Rockstar North). Ela descreve
trânsito por nós de estrada com características por motorista, redirecionamento em congestionamentos e densidade pelo
tipo de via. **Não achei nenhuma patente da Rockstar ou da Take-Two sobre deformação ou dano de veículos.**

### Cited Findings
- A patente "System and Method for Virtual Navigation in a Gaming Environment" foi registrada pela Take-Two com
  inventores David Hynd e Simon Parr. Os relatos falam em depósito em 29/10/2020 ou em outubro de 2020; outro dá abril de
  2019. As datas conflitam entre veículos de imprensa — [GameSpot](https://www.gamespot.com/articles/rockstar-npc-tech-patent-has-people-talking-about-gta-6/1100-6486479/);
  [TechRadar](https://www.techradar.com/news/gta-6-npc-drivers-could-be-a-whole-lot-smarter-than-their-predecessors);
  [80.lv](https://80.lv/articles/take-two-registers-a-new-patent-for-smarter-npcs);
  [VGC](https://www.videogameschronicle.com/news/gta6-npc-improvements-patent/).
- O conteúdo técnico, segundo a cobertura:
  - cada NPC pode "define its own specific characteristics for traversing the road nodes": aceleração, tempo e distância
    de frenagem, velocidade máxima e velocidade em curva;
  - os NPCs podem trocar de rota se uma via estiver congestionada;
  - a densidade de trânsito é preenchida de acordo com o tipo das vias, a proximidade entre elas e as ações dos NPCs;
  - os NPCs levam o trânsito em conta em perseguições em alta velocidade.

  Fontes: [TechRadar](https://techradar.com/uk/news/gta-6-npc-drivers-could-be-a-whole-lot-smarter-than-their-predecessors);
  [GamesRadar](https://www.gamesradar.com/a-new-system-designed-by-rockstar-devs-could-improve-npcs-in-gta-6);
  [Brobible](https://brobible.com/culture/article/grand-theft-auto-6-rockstar-patent).
- A patente não cita nenhum jogo. A ligação com o GTA 6 é inferência da imprensa — [GameSpot](https://www.gamespot.com/articles/rockstar-npc-tech-patent-has-people-talking-about-gta-6/1100-6486479/);
  [DSOGaming](https://www.dsogaming.com/news/new-patent-filed-by-rockstar-may-hint-at-significant-gta-6-npc-ai-traversal-improvements/).
- Há outras patentes da Take-Two sobre NPCs e mundo aberto (2021-2022) citadas como "mundo mais realista", sem detalhe
  de veículos nas fontes que vi — [MP1st](https://mp1st.com/news/take-two-interactive-patents-new-rockstar-games-developed-npc-tech-rumored-to-be-used-for-gta-6);
  [Mein-MMO](https://mein-mmo.de/en/gta-6-new-patents-indicate-a-better-and-more-realistic-open-world,639769).

### Inferences
- [ESPECULAÇÃO] "Características por motorista sobre nós de estrada" é exatamente um modelo de motorista parametrizado
  (como o IDM, Intelligent Driver Model, com aceleração, frenagem confortável e distância desejada) somado a um
  roteamento em grafo com custo dinâmico pelo congestionamento. Isso bate com os congestionamentos vistos nos trailers e
  com o "realistically dense" das análises.
- Equivalente na UE5: o MassTraffic tem parâmetros por tipo de veículo e segue faixas do ZoneGraph, mas não tem
  redirecionamento por congestionamento no grafo. Na Real Engine isso seria um campo SoA por agente (perfil do motorista)
  mais um A* incremental sobre o grafo de vias com custo por ocupação.

### Gaps
- Não consegui abrir o documento da patente (USPTO ou Google Patents) para confirmar número, data de depósito e
  reivindicações. A busca do Google Patents não está indexada pela ferramenta. Fica pendente conferir em
  patents.google.com com o responsável "Take-Two Interactive Software" e os inventores "Hynd" e "Parr".
- Não achei patentes sobre dano ou deformação de veículos, física de veículos, água e barcos, ou LOD de trânsito.

## 5. Comparação com o GTA 5 e o RDR2, análises de especialistas e fatos confirmados no lançamento (nov/2026)

### Takeaway
Em relação ao GTA 5, o salto confirmado é este: dirigibilidade com peso e suspensão (mais perto do GTA 4), deformação de
painéis com efeito na direção, terreno deformável herdado da lama do RDR2, trânsito muito mais denso em ruas estreitas,
e iluminação por RT (GI e reflexos) que muda a aparência dos carros à noite e na chuva. Tudo isso a 30 fps em 1440p nos
consoles.

### Cited Findings
- [ESPECIALISTA] A DF chama o GTA 6 de "landmark achievement in visual realism, simulation density and world design".
  O limite de 30 fps "compromises absolute responsiveness" — [ResetEra (trecho)](https://www.resetera.com/threads/grand-theft-auto-6-digital-foundry-tech-review-the-game-of-the-generation.1690775/).
- [ESPECIALISTA] Os 30 fps são um meio-termo deliberado, em parte por causa do sistema de cabelo — [VGTimes](https://vgtimes.com/gaming-news/175004-ray-tracing-realistic-physics-and-30-fps-digital-foundry-takes-a-technical-deep-dive-into-gta-6.html).
- [ESPECIALISTA] Sobre o desempenho, os resumos conflitam:
  - o PS5 Pro manteria 30 fps em cerca de 75 h e cairia a 24 fps em explosões intensas
    ([GTABoom](https://www.gtaboom.com/digital-foundry-finds-that-gta-6-indeed-runs-best-on-ps5-pro-b72f));
  - o Pro teria "unwavering 30fps lock", e os 24 fps seriam um caso extremo de teste de estresse
    ([ResetEra](https://www.resetera.com/threads/grand-theft-auto-6-digital-foundry-tech-review-the-game-of-the-generation.1690775/)).
- [ESPECIALISTA] O Series X fica no nível do PS5 base, com container VRR de 120 Hz. O Series S fica bem atrás — [GTABoom](https://www.gtaboom.com/digital-foundry-finds-that-gta-6-indeed-runs-best-on-ps5-pro-b72f);
  [TechPowerUp](https://www.techpowerup.com/355004/gta-6-reportedly-plays-best-on-ps5-pro-xbox-series-s-struggles).
- [ESPECIALISTA, pré-lançamento] A DF já previa 30 fps nos consoles pelo trailer 2 — [Purexbox](https://www.purexbox.com/news/2025/05/gta-6-tech-is-seriously-impressive-but-its-looking-like-a-30fps-game-on-console);
  [Rockstar Intel](https://rockstarintel.com/digital-foundry-reacts-to-gta-6-trailer-astonishing-detail-but-60fps-mode-unlikely).
  A Rockstar disse que o trailer 2 foi capturado num PS5 — [Notebookcheck](https://www.notebookcheck.net/PS5-was-behind-every-frame-of-GTA-6-Trailer-2-confirms-Rockstar.1012400.0.html).
- Comparação com a geração anterior: o GTA 4 usava a física e o Euphoria (NaturalMotion) com o dano mais realista; o
  GTA 5 simplificou para padrões predefinidos — [Sportskeeda](https://www.sportskeeda.com/gta/news-first-look-gta-6-car-deformation-physics-seemingly-found-trailer);
  [nkbgaming (BAIXA)](https://nkbgaming.com/blog/gta-6-vs-gta-4-physics-car-handling-body-roll-euphoria-ragdoll).
- [CONFIRMADO / análise] Carros como "terceiro lugar" (guardam armas, disfarces e roubos), roubo com keyless e
  rastreador, registro e venda no mercado negro — [Traxion (Eurogamer, TheSixthAxis)](https://traxion.gg/grand-theft-auto-6-reviews-praise-series-best-car-handling/).

### Inferences
- [ESPECULAÇÃO] A lama deformável e o heat haze vêm da RAGE do RDR2, que já deformava lama e neve e tinha o
  "mud/snow trail" no terreno. É provável que seja o mesmo mecanismo (deslocamento por heightmap dinâmico ou
  tessellation por zona).
- [ESPECULAÇÃO] O custo de CPU da densidade (população, física, streaming em alta velocidade) é parte do motivo dos
  30 fps, junto com o cabelo e o RT. A DF nota quedas no PS5 base na "direção intensa".
- Para a Real Engine (meta de 200 fps, `notas-dev/veiculos.md`, `multidao.md`): o paralelo direto seria
  - um anel de simulação completa (o modelo de veículo atual com mola e amortecedor) perto do jogador;
  - um anel intermediário de agentes SoA seguindo a spline da via (como o Crowd Spawner) pelo job system;
  - carros distantes como instâncias GPU-driven ou impostores octaedrais (já existem em `visao-distante.md`), com
    faróis como sprites emissivos;
  - o perfil por motorista da patente como campos SoA.

### Gaps
- Não há números de memória ou CPU por veículo, nem comparação direta, quadro a quadro, entre o trânsito do GTA 5 e o
  do GTA 6.
- Não confirmei se a análise técnica da DF fala de barcos e aviões, pintura ou dano em detalhe: o original não abriu.
- Não há análise pós-lançamento de datamine ou engenharia reversa (formatos, quantidade de veículos), o que é esperado
  com o jogo saído há poucos dias e sem versão de PC.
