# GTA 6: tecnologia de personagens e multidões (RAGE), com equivalentes na Unreal Engine 5

> Notas de pesquisa de 23/11/2026. Cada item vem marcado como **[CONFIRMADO]** (Rockstar, Take-Two ou documento
> oficial), **[ANÁLISE]** (Digital Foundry ou outro especialista), **[REVIEW]** (imprensa que jogou a versão final) ou
> **[ESPECULAÇÃO/VAZAMENTO]**. O jogo saiu em **19/11/2026** (PS5 / Xbox Series, sem PC). As reviews e a tech review
> da Digital Foundry saíram entre 17 e 20/11/2026.
> Ressalva de fonte: vários sites (GameSpot, VGC, NeoGAF, Eurogamer) devolveram 403 ao fetch. Por isso parte da
> análise da DF chega por resumos secundários (VGTimes, TweakTown, LibertyCity, RockstarIntel), e o vídeo original não
> foi checado aqui.

## 1. Personagens principais: pele, olhos, cabelo, tecido, rosto e captura

### Takeaway
O único item com suporte forte é o cabelo: a DF já lia cabelo por fios nos trailers e, na tech review final, descreve
um sistema de fios com física em **todos** os personagens, NPCs inclusive, caro o bastante para pesar contra um modo
60 fps. Suor irregular, cabelo que molha, pelos dos braços e roupa animada à parte do cabelo também foram vistos.
Subsuperfície da pele, olhos e o pipeline de captura facial não têm fonte técnica primária.

### Cited Findings
- **[ANÁLISE, Trailer 2, mai/2025]** Na leitura da DF, o cabelo usa um "new strand-based system" de ótima qualidade, que
  pede "significant resources and dynamic optimization". Há falhas visíveis de cabelo. Cabelo e roupa são animados
  separadamente, e o suor visível "greatly enhancing realism" —
  [LibertyCity (resumo da DF)](https://libertycity.net/news/gta-6/8997-digital-foundry-analyzes-gta-6s-second.html)
- **[ANÁLISE, mai/2025]** John Linneman (DF) elogia o movimento do cabelo quando Lucia joga Jason, sinal de fios, e os
  pelos do braço de Jason: "I don't think I've ever seen such detailed arm hair". A roupa se move em parte de forma
  independente, e o suor forma padrões irregulares e naturais. Alex Battaglia acha os personagens muito melhores que os
  do RDR2, cujos secundários eram "cartoony and weird" —
  [RockstarIntel](https://rockstarintel.com/digital-foundry-reacts-to-gta-6-trailer-astonishing-detail-but-60fps-mode-unlikely)
- **[ANÁLISE, resumo secundário]** A Rockstar nunca usou o termo "strand hair" oficialmente. "Fios" é inferência da DF
  pelo comportamento na tela. O mesmo resumo cita subsuperfície amaciando as bordas do rosto e diz que os pelos do braço
  não são exclusivos dos protagonistas. É um site de fãs, confiança média —
  [Leonida Interactive (wiki de fãs)](https://leonidainteractive.com/wiki/guides/character-tech/);
  [GTA Intel](https://gtaintel.com/news/gta-6-strand-hair-and-detail-tech)
- **[ANÁLISE, tech review final, 18/11/2026]** A DF descreve um "physics-based strand hair system" usado em todos os
  personagens, inclusive NPCs. Os fios reagem a movimento, vento e água. Esse custo, multiplicado por muitos personagens
  ao mesmo tempo, é uma das razões de não haver 60 fps —
  [VGTimes (resumo da DF)](https://vgtimes.com/gaming-news/175004-ray-tracing-realistic-physics-and-30-fps-digital-foundry-takes-a-technical-deep-dive-into-gta-6.html)
- **[REVIEW, Kotaku, 18/11/2026]** "Hair gets wet", "Sweat builds up on backs". Os personagens podem "gain weight, get
  tired" (simulação de vida). A review elogia as atuações, mas não comenta a animação facial —
  [Kotaku Review](https://kotaku.com/grand-theft-auto-6-the-kotaku-review-2000757842)
- **[CONFIRMADO]** Segundo a Rockstar, o Trailer 2 foi capturado inteiro num PS5 comum (não Pro) e mistura gameplay em
  tempo real com cutscenes no motor —
  [Notebookcheck](https://www.notebookcheck.net/PS5-was-behind-every-frame-of-GTA-6-Trailer-2-confirms-Rockstar.1012400.0.html)
- **[ANÁLISE]** Na leitura da DF, o Trailer 2 renderiza em 1152p (2560×1152 em letterbox) com FSR1, a 30 fps —
  [LibertyCity](https://libertycity.net/news/gta-6/8997-digital-foundry-analyzes-gta-6s-second.html)

### Inferences
- **Cabelo**: há fios com física em todos os personagens, com orçamento "dinâmico" pela distância, mas a DF não diz se
  os NPCs distantes caem para cards. O equivalente direto é o **Groom da UE5**, que passa de fios a cards e depois a
  malha pela distância (LODs de groom). Na Real Engine, isso corresponde ao componente Groom de `render.md` (fios
  dinâmicos com física na GPU). Para "fios em todos os NPCs", o essencial é o LOD de groom com orçamento global.
- **Pele**: a subsuperfície e o suor/umidade em camadas pedem o equivalente ao Subsurface Profile ou Substrate da UE5,
  mais máscaras dinâmicas de molhado/suor por região do corpo. O suor que "acumula nas costas" sugere um mapa de
  acúmulo por personagem que evolui com o esforço. Na Real Engine, isso é a pele com subsuperfície e o nó Wet Surface
  (suor) de `materiais.md`.
- **Rosto e corpo**: o equivalente mais próximo é o **MetaHuman** (rig facial por blendshapes/correctives e grooms).
  Nenhuma fonte encontrada descreve o rig facial da Rockstar.
- **Tecido**: "roupa animada separada do cabelo" e "move em parte de forma independente" apontam para simulação
  secundária, ou seja, Chaos Cloth ou ML Cloth na UE5. Não se sabe se é tecido simulado ou ossos dinâmicos.

### Gaps
- Nada primário (GDC, SIGGRAPH, Rockstar) sobre subsuperfície, poros, olhos (refração, cáustica, umidade) ou o modelo
  de shading do cabelo.
- Pipeline de captura facial e de performance: não achei fonte. Atores, estúdio e uso de 4D/volumétrico seguem sem
  confirmação.
- Se os NPCs distantes usam cards ou malha em vez de fios: a DF fala de LOD "imperceptível", mas sem detalhar o cabelo.

## 2. Multidões de NPCs: densidade, variedade, LOD, comportamento, Euphoria e ragdoll

### Takeaway
A versão final confirma multidões muito densas e cheias de rotinas. O custo delas está na CPU, não na GPU: é o motivo
central dos 30 fps em todos os consoles. Num teste, um tiro numa multidão de aeroporto troca dezenas de NPCs para rotinas
de IA mais caras e derruba o quadro para a casa dos 20 fps. Os pedestres aparecem até o horizonte, com transições de
LOD descritas como imperceptíveis. A quantidade de modelos únicos e o uso de Euphoria não foram confirmados.

### Cited Findings
- **[REVIEW, Kotaku]** Numa única estação de bonde, "over 100 different NPCs milling about, all unique and all moving".
  Testemunhas denunciam a posição do jogador quando o roubo é filmado. Homens assediam Lucia na rua. As falas de um NPC
  mudam conforme a perseguição. O jogo tem ônibus e trens com horários e passageiros e lojas 24 h (a barbearia só tem
  funcionários às 3 h). Há eventos aleatórios, como um urso-polar que foge do caminhão —
  [Kotaku Review](https://kotaku.com/grand-theft-auto-6-the-kotaku-review-2000757842)
- **[REVIEW, Kotaku]** O celular tem um app de rede social "Instagram-like", mensagens e ligações. Notícias e posts
  repercutem as ações do jogador (o urso-polar, um golpe de criptomoeda). Uma amiga de Lucia manda mensagem para saber
  como ela está —
  [Kotaku Review](https://kotaku.com/grand-theft-auto-6-the-kotaku-review-2000757842)
- **[ANÁLISE, DF, 18/11/2026]** Segundo a DF:
  - as multidões de pedestres ficam visíveis até o horizonte e as transições de LOD são "virtually imperceptible";
  - os pedestres têm alta autonomia, reagem a eventos e procuram abrigo em tiroteios;
  - a polícia sai de delegacias espalhadas pelo mapa e precisa ver o suspeito antes de perseguir;
  - a reação ao tiro depende de onde o alvo é atingido (manca, larga a arma, segura o ferimento);
  - os 30 fps são um compromisso deliberado em favor da simulação.

  [VGTimes](https://vgtimes.com/gaming-news/175004-ray-tracing-realistic-physics-and-30-fps-digital-foundry-takes-a-technical-deep-dive-into-gta-6.html)
- **[ANÁLISE, DF, Oliver Mackenzie, 20/11/2026]** No Xbox Series X, andar por uma multidão de aeroporto fica em 30 fps.
  Um tiro para o alto faz dezenas de NPCs trocarem para rotinas de IA mais avançadas e fugirem, e "the frame rate dives
  into the 20s and stays there". Carros explodindo, batidas em alta velocidade e streaming dirigindo rápido também
  derrubam o quadro em todos os consoles, que usam CPUs Zen 2 parecidas. Os limites de CPU "can't be removed without
  dramatically altering the game itself", e nem 200% da performance de um console bastaria num PC nas cenas pesadas —
  [TweakTown](https://www.tweaktown.com/news/114810/gta-6-hitting-a-consistent-60-fps-even-with-pc-hardware-is-doubtful/index.html)
- **[ANÁLISE, DF Direct, Richard Leadbetter, 17/11/2026]** Os 33,3 ms do quadro precisam cobrir tráfego, multidões,
  física e "systemic chaos". Um modo 40 fps (25 ms) pediria "a huge amount of optimization or cutbacks", e 60 fps é
  "not viable" —
  [TweakTown](https://www.tweaktown.com/news/114744/digital-foundry-says-gta-vis-cpu-demands-make-a-40-fps-mode-on-ps5-pro-too-difficult/index.html)
- **[ANÁLISE]** No PS5 Pro, a DF vê 30 fps estáveis com frame pacing perfeito, PSSR e sombras por ray tracing. No PS5
  base, o jogo cai para a casa dos 25 fps em tiroteios longos com muitas explosões. Os dois consoles têm GI e reflexos
  por RT —
  [TweakTown](https://www.tweaktown.com/news/114763/gta-6-deemed-the-single-most-visually-striking-game-of-the-generation-by-tech-experts/index.html);
  [VGTimes](https://vgtimes.com/gaming-news/175004-ray-tracing-realistic-physics-and-30-fps-digital-foundry-takes-a-technical-deep-dive-into-gta-6.html)
- **[ANÁLISE, Trailer 2, secundário]** Uma praia no Trailer 2 tem centenas de pessoas, cada uma com cabelo, pele e
  roupa. O truque seria gastar o orçamento inteiro só com as que estão perto da câmera. A fonte é de fãs, confiança
  baixa a média —
  [Leonida Interactive](https://leonidainteractive.com/wiki/guides/character-tech/)
- **[ANÁLISE, Trailer 1, dez/2023]** A DF fez um DF Direct do Trailer 1, mas só achei a chamada, não o conteúdo
  sobre a praia —
  [PureXbox](https://purexbox.com/news/2023/12/digital-foundry-breaks-down-grand-theft-auto-6-trailer-1)
- **[ESPECULAÇÃO, pré-lançamento]** Sobre Euphoria: o GTA V teria contido o Euphoria pelo hardware, o RDR2 seria o
  auge dele na Rockstar, e a volta no GTA 6 era plausível, mas não confirmada —
  [GTA Zone](https://gta-zone.com/news/gta-6-euphoria-ragdoll-physics/)
- **[REVIEW]** A review da Kotaku não usa o termo "ragdoll". Ela diz que inimigos atingidos no braço "realistically fall
  back" —
  [Kotaku Review](https://kotaku.com/grand-theft-auto-6-the-kotaku-review-2000757842)

### Inferences
- **Arquitetura provável**: as multidões funcionam como no **Mass Entity / Mass AI da UE5**. Há muitos agentes baratos
  (LOD de simulação), e só os próximos ou "ativados" sobem para a IA completa. O tiro no aeroporto mostra essa subida
  em massa como uma conta de CPU que não é amortizada. Lição para uma engine: limitar quantos agentes podem ser
  promovidos por quadro (fila com orçamento) e espalhar a promoção ao longo de vários quadros.
- **LOD de animação**: "pedestres até o horizonte" e "LOD imperceptível" pedem a mesma coisa que o **Animation Budget
  Allocator** e o **Mass crowd LOD da UE5** fazem: taxa de atualização da animação por distância, animação por vertex
  animation/instancing ao longe e impostores no limite. Não há confirmação de impostores no GTA 6. Na Real Engine, isso
  corresponde a `multidao.md` (agentes SoA, desenho por compute, LOD com orçamento) e aos impostores de
  `visao-distante.md`.
- **Variedade**: "100 NPCs únicos numa estação" indica montagem modular (cabeça, corpo, roupa, cabelo e cores) em vez de
  modelos inteiros, como o **Mutable / Customizable Object da UE5** ou MetaHumans com corpos e roupas trocáveis.
- **Reações físicas por região do corpo** (mancar, largar a arma, segurar o ferimento) combinam com o Euphoria/NaturalMotion
  dos jogos anteriores da Rockstar, mas nenhuma fonte final confirma o nome. O equivalente na UE5 é Physical
  Animation + Physics Control / Chaos Ragdoll. Na Real Engine, é o ragdoll ativo de `ikrig.md` e o active ragdoll
  pendente na memória.
- **Rotinas e celular**: lojas 24 h, transporte com horário e posts reagindo ao jogador pedem uma camada de
  agendamento e "eventos do mundo" publicados num barramento que a UI do celular e a IA consomem. Na UE5, isso seriam
  Smart Objects + State Tree + Mass, e não existe um sistema pronto de "rede social".

### Gaps
- Número de modelos ou combinações únicas de NPC: só há a frase anedótica da Kotaku.
- Contagem máxima de NPCs simultâneos: só há "dezenas" e "centenas" vagos.
- Euphoria ou NaturalMotion no jogo final: não há confirmação oficial nem da DF nas fontes lidas.
- Vazamento de 2022: não procurei nem cito detalhes. Qualquer dado dele teria confiança baixa.

## 3. Tecnologia de animação: motion matching, animação procedural e patentes da Rockstar/Take-Two

### Takeaway
A patente mais relevante é a US 11,620,781 B1 (Take-Two, prioridade de out/2019, concedida em abr/2023). Ela descreve
locomoção montada por blocos hierárquicos dirigidos por dados (arquétipo, tipo de movimento, bloco de animação), um
blackboard de variantes e tabelas de transição. Não descreve motion matching. As vagas da Rockstar North pediam
experiência com motion matching e com ML para controle de personagem, mas o uso no GTA 6 não está confirmado.

### Cited Findings
- **[CONFIRMADO, Google Patents]** US 11,620,781 B1, "System and method for virtual character locomotion":
  - assignee Take-Two Interactive;
  - inventores Tobias Kleanthous, Mike Jones, Chris Swinhoe, Arran Cartie, James Stuart Miller, Sven Louis Julia van
    Soom;
  - depositada em 23/10/2020, prioridade de 25/10/2019, concedida em 04/04/2023.

  O método:
  - escolhe um **archetype block**, que agrupa **motion type blocks**;
  - cada motion type block agrupa **motion animation blocks** de um mesmo estilo;
  - cada motion animation block é a unidade básica de movimento, descrita por atributos;
  - a sequência dos motion type blocks sai dos movimentos pedidos pela lógica do jogo;
  - um **blackboard** alimentado pelo código (não pelos dados) dá variantes que viram critério de seleção do bloco;
  - os blocos são independentes da lógica do jogo.

  Das reivindicações dependentes:
  - um segundo arquétipo pode mudar a animação conforme a história avança;
  - um arquétipo de fallback herda blocos não substituídos;
  - os blocos são definidos em XML;
  - há tabelas de transição (taxa de blend, sincronização, filtragem) e edição em tempo real no jogo.

  A patente não fala em motion matching —
  [Google Patents US11620781B1](https://patents.google.com/patent/US11620781B1/en)
- **[CONFIRMADO, patente coberta pela imprensa]** "System And Method For Virtual Navigation In A Gaming Environment"
  é da Take-Two, com inventores David Hynd (diretor técnico da Rockstar North) e Simon Parr (líder de IA). Ela trata da
  direção de NPCs, que pondera limite de velocidade, dirigibilidade, velocidade máxima, curvas e frenagem, troca de
  rota quando a via está bloqueada e densidade de tráfego por tipo de via. A data diverge entre as fontes (abr/2019 ou
  out/2020), o número não foi verificado e o texto não cita nenhum jogo —
  [PC Gamer](https://pcgamer.com/take-two-interactive-patent-could-point-to-a-bigger-better-world-for-the-next-grand-theft-auto);
  [80.lv](https://80.lv/articles/take-two-registers-a-new-patent-for-smarter-npcs);
  [GameSpot](https://www.gamespot.com/articles/rockstar-npc-tech-patent-has-people-talking-about-gta-6/1100-6486479/);
  [VGC](https://www.videogameschronicle.com/news/gta6-npc-improvements-patent/)
- **[CONFIRMADO, vagas, datas variadas]** Algumas vagas da Rockstar, num resumo de busca (confira as fontes):
  - AI/Gameplay Programmer em Oakville: sistemas de movimento de jogador e NPC "using AI and Animation technology",
    animação procedural e IK;
  - outra vaga de AI/Gameplay: "systems to populate and simulate large worlds efficiently", com população, streaming
    e multithreading;
  - Rockstar San Diego: "ambient character behaviors, group behaviors, combat behaviors";
  - World & Population Designer: "create and author dynamic ambient content";
  - existe uma vaga "Animation Programmer, Procedural Motion".

  [Greenhouse Rockstar](https://job-boards.greenhouse.io/rockstargames/jobs/6240029003);
  [Hitmarker](https://hitmarker.net/jobs/rockstar-games-ai-programmer-807505);
  [The Muse, World Population](https://www.themuse.com/jobs/rockstargames/designer-world-population);
  [Hitmarker, Animation Programmer (expirou em 27/02/2024)](https://hitmarker.net/jobs/rockstar-games-animation-programmer-all-levels-1376743)
- **[CONFIRMADO, vaga de 2021]** AI/Gameplay Programmer na Rockstar North (Edinburgh), que pede experiência com
  **motion matching** e **machine learning** para controle e animação de personagem. A informação vem do resumo de
  busca, porque a página deu 403 —
  [Bright Network](https://www.brightnetwork.co.uk/graduate-jobs/rockstar-games/aigameplay-programmer-edinburgh-2021)
- **[ESPECULAÇÃO, blogs]** Circulam números como "600.000+ animações" e números de patente como US 11,684,855 B2 e
  US 10,685,152 B2 para "NPC intelligence". São sites de baixa confiança e não verifiquei esses dados. A mesma página
  diz que a Take-Two afirmou não usar IA generativa no GTA 6 —
  [GenAI Magazine](https://thegenaimagazine.com/gta-6-npc-ai-600000-animations-explained&gsessionid=2yKepbBZo6Pvr0yztZ2ASX44fzpBPBbfMi0MlAXhNKYJdfLd2hFGBQ);
  [Worldeka](https://worldeka.com/gta-6-npc-ai-behavior-and-interaction-system-inside-rockstar-games-most-realistic-open-world-yet-2026-deep-dive/)

### Inferences
- A patente US 11,620,781 é um "sistema de locomoção por arquétipos" dirigido por dados, perto dos Anim Layers,
  Linked Anim Graphs e Chooser Tables da UE5. O **Motion Matching / Pose Search da UE5** é uma abordagem diferente, que
  busca na pose em vez de sequenciar blocos. É provável que a Rockstar combine as duas: blocos e arquétipos para
  variedade em massa (NPCs) e motion matching nos protagonistas, como sugere a vaga de 2021, mas não há confirmação. Na
  Real Engine, isso corresponde ao Motion Matching de `animacao.md` e às Chooser/Anim Layers que ainda faltam.
- A patente de navegação de veículos corresponde, na UE5, a ZoneGraph + Mass Traffic (o City Sample) e, na Real Engine,
  aos sistemas de `ia.md` e `veiculos.md`.

### Gaps
- Nenhuma palestra da Rockstar sobre animação ou IA do GTA 6 na GDC ou SIGGRAPH foi encontrada.
- Não procurei patentes sobre espelhos ou reflexos de personagens, e não houve tempo para uma busca ampla no Google
  Patents.
- Não achei entrevistas com Strauss Zelnick sobre NPCs ou personagens. Só aparece a confiança dele na data de 2026 —
  [GamesRadar](https://www.gamesradar.com/games/grand-theft-auto/gta-6-delayed-6-more-months-as-rockstar-commits-to-november-2026-and-says-sorry)

## 4. Fauna e o estado de lançamento (fatos confirmados após 19/11/2026)

### Takeaway
O jogo saiu em novembro de 2026, com reviews muito positivas (GameSpot deu 10). A fauna é específica da Flórida, com
mais de 250 animais no menu de progresso, caça, pesca e zoológico. Não há detalhe técnico, nem da DF, sobre a animação
ou a IA dos animais.

### Cited Findings
- **[CONFIRMADO]** O jogo foi adiado de 26/05/2026 para 19/11/2026, e Zelnick disse estar "highly confident" na nova
  data —
  [GamesRadar](https://www.gamesradar.com/games/grand-theft-auto/gta-6-delayed-6-more-months-as-rockstar-commits-to-november-2026-and-says-sorry);
  [VGC](https://www.videogameschronicle.com/news/grand-theft-auto-6-has-been-delayed-again-to-the-end-of-2026/)
- **[REVIEW]** GameSpot deu 10 ("Setting The Bar For Open Worlds Impossibly High"). Metacritic e Push Square também
  avaliaram. A PC World diz que os consoles seguram o jogo —
  [GameSpot](https://www.gamespot.com/reviews/grand-theft-auto-6-review/);
  [Metacritic](https://www.metacritic.com/game/grand-theft-auto-vi/);
  [PC World](https://www.pcworld.com/article/3292551/gta-6-review.html)
- **[CONFIRMADO/REVIEW]** A Rockstar anunciou mais de 170 espécies, e o menu de progresso mostra mais de 250 animais.
  Há jacarés, flamingos, peixes-boi, iguanas-verdes, sapos-cururu e macacos-rhesus, além de zoológico, pesca e caça —
  [GamesRadar](https://www.gamesradar.com/games/grand-theft-auto/gta-6-animals/);
  [Yahoo Tech](https://tech.yahoo.com/gaming/articles/alligators-pet-snakes-dog-jet-200257949.html);
  [Red Bull](https://www.redbull.com/gb-en/gta-6-wildlife)
- **[REVIEW, Kotaku]** O pântano está "teeming with creatures, big and small", com uma cobra gigante carregando um
  corpo. Uma caçada rendeu US$ 450 —
  [Kotaku Review](https://kotaku.com/grand-theft-auto-6-the-kotaku-review-2000757842)

### Inferences
- Mais de 250 espécies com IA própria indicam sistemas de comportamento por arquétipo de animal: o mesmo esquema de
  arquétipos da patente de locomoção vale para quadrúpedes, aves e peixes. Na UE5, isso seria Mass + State Tree com
  animações por espécie. Na Real Engine, entraria em `ia.md` e no active ragdoll pendente para quadrúpedes.

### Gaps
- Nenhuma análise técnica da animação dos animais (IK de quadrúpedes, motion matching, pelagem) foi encontrada.
- As análises da DF citadas acima vêm de resumos secundários. A tech review original da DF na Eurogamer ou no YouTube
  não foi lida diretamente, e a busca por ela pela Eurogamer não retornou nada.
