# Demos e assets CC0 (sugestões #29, #31, #34, #43, 2026-11-04)

## Assets
- O manifesto `data/demo/manifest.tsv` foi gerado das APIs do Poly Haven (modelos glTF 1k) e do ambientCG (texturas
  1K-JPG). O `data/demo/LICENSES.txt` lista autor, link e licença (CC0 1.0) de cada asset. Os dois ficam no git; os
  arquivos não (~180 MB baixados).
- O alvo `demo_assets` (`tools/fetch_demo_assets.cmake`) baixa em todo build para `Release/DemoContent`:
  - o que já está lá com o tamanho certo é pulado;
  - dos zips do ambientCG só ficam Color, NormalGL, Roughness e AO;
  - sem internet, só um aviso.
  - O `tools/make_dist.ps1` põe a pasta no pacote.
- Total extraído: 127 MB.
- Ficaram de fora as árvores realistas do Poly Haven (pinheiro 914 MB, abeto 465 MB). As árvores das demos são as do
  Foliage da engine; o RPG usa tocos e troncos (tree_stump_01, dead_tree_trunk).

## Kit (`src/demo_kit.inc`)
- `kit_material(demo, id, tile_cm, tint, rough)`: um conjunto do ambientCG vira um material do projeto em
  `Content/Demo/<demo>`, com nós Image Texture (cor × AO, rugosidade, normal OpenGL → Normal Map). É World Aligned
  quando `tile_cm > 0`.
- `kit_prop` / `kit_place` / `kit_done`: o modelo é importado uma vez (as peças viram Static Meshes com materiais PBR).
  Cada colocação copia as peças, giradas e escaladas juntas, com a base em z. Com `solid`, ganha uma caixa de colisão
  do tamanho dos bounds.
- `kit_licenses`: copia o LICENSES.txt para `Content/Demo` do projeto.
- Sem DemoContent tudo responde vazio, e as demos voltam às formas coloridas.

## Modos de jogo nativos (`src/demo_play.c`)
- O Game Mode do projeto (`DemoFighting`, `DemoHorror`, `DemoRPG`) liga a lógica da demo em C, como os samples da
  Unreal fazem em C++. Ela roda igual no PIE e no jogo empacotado. Pontos de entrada:
  - `project_begin_play` → `demo_play_begin`;
  - `bp_tick` → `demo_play_tick`, depois do movimento;
  - `bp_player_camera` → `demo_play_view`, a câmera própria.
- **Correção nos testes**: com `--new-project` mais `--play`, o Play começava no nível vazio, e a abertura do projeto
  (entre quadros) o parava. Os testes de demos e templates mediam o editor parado. Agora o Play espera o projeto abrir
  (`play_after_open`).
- `REALENGINE_DEMO_AUTOPLAY=1` (só para os testes): o jogador joga pela mesma IA da demo.

## Luta (sugestão #34)
- **Animações** (`src/anim_fight.inc`): procedurais, no estilo dos clipes do Mannequin. Guarda ortodoxa, passo de luta,
  jab, cruzado (o calcanhar de trás sobe), gancho (carrega e gira), uppercut (afunda e sobe), chute circular (o joelho
  arma, a perna de apoio por IK), chute aéreo, defesa, reação ao golpe, esquiva, rolamento, KO e vitória. Cada golpe
  começa e termina na guarda, então os combos encadeiam sem salto. Há uma montagem por golpe (DefaultSlot, corpo
  inteiro) e o **ABP_Fighter** (guarda parada, passo até 160 cm/s, jog).
- **Jogo** (`src/demo_fight.inc`):
  - Melhor de 3 rounds de 60 s.
  - LMB: jab → cruzado → gancho → uppercut, em cadeia enquanto o golpe dura. RMB: chute. Espaço + LMB: chute aéreo.
  - Shift: defesa (15% do dano, sem stagger). Q: esquiva. Ctrl: rolamento. Os dois deixam intocável enquanto duram.
  - Cada golpe tem startup, janela ativa (alcance e frente), recuperação, hitstun e knockback. Golpes a menos de 1 s um
    do outro contam como combo.
  - **IA**: pensa a cada 0,12–0,25 s (o tempo de reação). Avança de lado, recua, circula, defende, esquiva ou rola o
    que vê vir, pune golpe errado, faz combos e chuta da borda do alcance.
  - Câmera de luta lateral, que afasta com a distância. HUD em UMG (`WBP_FightHUD`): barras, nomes, rounds, timer,
    contador de combo, faixa central, controles.
- **Mapa**: um clube de luta subterrâneo, galpão de 28 m.
  - Materiais do ambientCG em World Aligned: concreto no piso, chapas de aço nas paredes, lona no ringue.
  - Props do Poly Haven: luminárias industriais sobre o ringue, fogões de tambor nos cantos, caixotes militares, baús de
    ferramentas. Névoa leve.
  - A cerca de arame ficou de fora: 26 peças por módulo davam 728 draws.
  - Só as duas luminárias principais fazem sombra por raio.
- **Medidas** (RTX 3060, 2560×1417, jogando): 224 fps, GPU 4,1 ms, CPU 1,4 ms, 166 draws. Antes da cerca e das sombras
  saírem: 103 fps.
- **Teste** `demo_fighting`: autoplay por 6 s. Golpes acertam, montagens tocam, a IA ataca, > 180 fps.
  (2026-11-23) A medida começava no primeiro quadro depois de abrir o projeto: o `--new-project --play` leva ~5 s, mais
  que o segundo de aquecimento do início, e os primeiros quadros do nível (528 + 53 ms: envios, BLAS, pipelines RE_EXT)
  entravam nos 6 s (pior quadro 408-434 ms, 148-164 fps). Agora a medida espera 1 s depois de abrir, como no início
  (`main.c`, o bloco do `pending_project`): pior quadro 12 ms, 173 → 194 fps escondido com a GPU livre.

## Terror (sugestão #29)
- **Mapa**: uma clínica abandonada à noite, feita com paredes do kit de protótipo (colidem e navegam) e portas onde a
  planta pede. Cômodos: foyer, biblioteca, corredor, enfermaria, almoxarifado, escritório, sala do gerador e o salão da
  saída, atrás de um portão elétrico.
  - Materiais do ambientCG (World Aligned) por cômodo: papel de parede velho, reboco descascando, tábuas gastas, ladrilho
    de clínica, concreto, ferrugem.
  - Móveis do Poly Haven: camas de ferro, cadeira de rodas, caixa médica, estantes, relógio de pêndulo, armários,
    gerador, lamparinas acesas.
- **Puzzles** (`src/demo_horror.inc`): a cadeia que faz pensar.
  - Três fusíveis: um na enfermaria e um no almoxarifado. O terceiro está trancado no relógio. O diário diz "às três da
    manhã... o relógio velho ainda lembra"; E avança os ponteiros, e às 3 a caixa abre.
  - Com os fusíveis no gerador, os quatro disjuntores vão na ordem do cartão do escritório (II, IV, I, III). A ordem
    errada desarma o painel e faz barulho.
  - A energia volta: as luzes da casa acendem, o portão abre, e a criatura ouve tudo e fica mais rápida.
- **Ameaça e controles**: a criatura caça por visão e ruído (correr com Shift é alto). Pego, você acorda no último
  checkpoint e mantém o que achou. F: lanterna (nativa, sem tocar no ECS a cada quadro). E: interagir; o prompt mostra
  o objeto. Bilhetes abrem como uma página na tela.
- **HUD** (`WBP_HorrorHUD`): objetivo, fusíveis, prompt, mensagens, página do bilhete, faixa final.
- **Correção no renderer**: um slot quente sem malha (câmera, boom, luz) que não tinha e continua sem instância não marca
  mais `static_changed`. Antes, em primeira pessoa, o boom e a câmera tocados a cada quadro faziam o TLAS estático dar
  refit todo quadro.
- **Medidas** (RTX 3060, 2560×1417):
  - Jogando no Médio: 168 fps, GPU 4,7 ms.
  - Épico com todos os charts do cache (`demo_horror_flicker`): ~120 fps, cintilação 0,05% vista do foyer. A câmera do
    editor agora começa dentro do foyer; de cima, o teste via as nuvens noturnas animadas pela janela do céu.
- **Testes**:
  - `demo_horror`: autoplay resolve a cadeia inteira (fusíveis, relógio, bilhetes, gerador, disjuntores, portão,
    saída), > 180 fps.
  - `demo_horror_flicker`: < 0,2% de cintilação, > 100 fps no Épico.

## RPG (sugestão #31)
- **Mapa: Oakvale**, uma vila de ~70 × 50 m.
  - Ruas e praça de paralelepípedo, grama, terra no campo e no acampamento (ambientCG, World Aligned).
  - Nove casas de tijolo com telhado de telha em duas águas, cada uma uma sala do kit com a porta para a rua; a do
    Ancião é a maior.
  - Na praça: a fogueira de pedra e quatro barracas de mercado (balcão, toldo listrado, mesa, cesto, caixote).
  - Barris, baú, mesas e cadeiras, baldes e lampiões de rua (Poly Haven).
  - Acampamento do lenhador com troncos mortos e tocos, plantação de trigo, árvores em volta.
- **Jogo** (`src/demo_rpg.inc`):
  - **50 itens à venda** em quatro lojas: ferreiro 20, mercearia 12, alquimista 10, mercador 8. Mais três itens de
    trabalho: tora, feixe, encomenda.
  - **Loja em UMG** (`WBP_Shop`): as mercadorias à esquerda (clique compra; sem ouro o botão fica desabilitado), a sua
    bolsa à direita (clique vende pela metade do preço), o ouro e Fechar. Os cliques chegam ao C por um gancho novo da
    UMG, `ui_set_native_events`.
  - **Trabalhos**:
    - Lenhador: empresta o machado; três golpes por tora; 6 de ouro cada.
    - Fazendeira: 4 por feixe; o trigo cresce de novo.
    - Mensageiro: uma encomenda para um morador; 25 na entrega. Os três se repetem.
  - **Missão "O Escudo de Oakvale"**: falar com o Ancião → levar uma Poção de Cura ao moleiro doente → juntar 80 de
    ouro → levar um Escudo de Ferro ao Ancião → recompensa (anel de prata e ouro).
  - **Diálogos na tela**: caixa com nome e fala, E avança. Nada de Print String.
  - **HUD** (`WBP_RPGHUD`): a missão e o passo atual, o ouro, as toras e feixes e a encomenda, o prompt (E) e avisos.
- **Medidas** (RTX 3060, 2560×1417, jogando): Médio 166 fps (GPU 5,9 ms), Alto 133 fps. O maior custo é a sombra por
  raio do sol sobre a vista toda (1,9 ms). Épico com os 572 charts do cache: ~90–110 fps.
- **Testes**:
  - `demo_rpg`: autoplay faz a missão, os três trabalhos e a loja (missão no passo 4, uma entrega), > 150 fps no Médio.
  - `demo_rpg_flicker`: < 0,3% de cintilação (medido ~0,24%: grama texturizada e 10 moradores parados e animados na
    vista), > 70 fps no Épico com todos os charts (77–110 medidos, conforme a GPU está livre ou não).

## Casa assombrada (2026-11-06, `--scene haunted`, `src/haunted_house.inc`)
Pedido: uma cena de terror realista para medir a engine, acima de 100 fps. O pedido era refazer uma cena de um jogo
comercial (personagem e monstro); isso não se faz. Esta é uma cena original de survival horror: a casa da cena de GI
(10 × 8 m, dois cômodos e uma porta) como casa vitoriana abandonada, à noite.
- **Assets, todos CC0, em 2K:**
  - 27 modelos do Poly Haven e 9 materiais do ambientCG.
  - Ficam ao lado do projeto, em `<Real Engine Projects>/CasaAssombrada_fontes` (o `REALENGINE_HAUNTED_SOURCES` muda o
    lugar), com o `LICENSES.txt`. Não entram na build: são 280 MB.
  - Foram baixados por script, pelas APIs do Poly Haven (`/files/<id>`, glTF 2k com as texturas) e do ambientCG (zip
    2K-JPG: Color, NormalGL, Roughness, AO).
- **O kit (`demo_kit.inc`)** ganhou `kit_source(root, hi)`: a pasta de origem e a resolução (2K: materiais assados em
  2048). As demos continuam na DemoContent em 1K.
- **A cena:**
  - **Paredes** de blocos, com janelas (moldura, cruzeta e peitoril), papel de parede oliva desbotado sobre lambri de
    madeira escura até 95 cm, e porta com batente.
  - **Lareira** de tijolo com brasas; piso de tábuas escuras; teto de reboco; tapete vinho; terra e troncos mortos lá
    fora.
  - **Sala:** mesa de jantar, cadeiras, lustre, poltrona, cadeira de balanço, estantes, relógio de pêndulo, busto,
    espelho, castiçais, relógio de lareira, retratos, livros, garrafas e vela.
  - **Quarto:** cama gótica, criado-mudo com lamparina, armário, cômoda, mala, cadeira de rodas e quadro. Ratos pelo
    chão.
  - **Personagens** da própria engine: a Mannequin feminina agachada atrás da mesa (`MM_CrouchIdle`) e uma figura
    esticada (Mannequin masculino 0,82 × 0,82 × 1,3) andando devagar na porta.
  - **Luz:** lua fria pelas janelas na névoa volumétrica; lustre de velas (sombra por raio); lareira; velas no console
    e na mesa; lamparinas. Pós-processo com exposição fixa, frio, dessaturado, vinheta e granulação. Sem nuvens.
  - O nível é salvo em `Content/Maps/CasaAssombrada.rlevel` como mapa inicial do editor e do jogo, no Alto. O projeto
    é de Primeira Pessoa (Play anda pela casa).
- **Para passar de 100 fps** (Play, 1440p, mínimo da GPU; começou em 20,5 ms):

| Mudança | Ganho |
|---|---|
| Lareira, lamparina e lustre como fontes pontuais (1 raio exato; chama de vela tem ~1 cm) | 7,6 ms com três de área; o lustre suave sozinho custava 1,7 ms |
| Sem nuvens | 1,6 ms |
| Alcance das luzes do tamanho dos cômodos | ~0,3 ms |
| AO de contato: o oclusor lido só se estiver ao alcance (gi.md) | 2,1 → 1,3 ms |
| A cúpula do lustre sem sombra | (tirou uma mancha preta do teto) |

  Medido sem impacto: os reflexos (0,2 ms), a névoa volumétrica (0,4 ms) e o híbrido da lua (0,3 ms).
- **Resultado** (Play, RTX 3060, 2560×1417): **Alto 120 fps** (8,3 ms por quadro, mínimo da GPU 7,7 ms), **Épico 101
  fps**. 0,4 M triângulos, 271 instâncias, 7 luzes. Validação sem erro.

## Metrô abandonado (2026-11-19, `--scene metro`, `src/metro_scene.inc`)
Pedido: refazer em tempo real, "1:1" e com ótimo FPS, a imagem do viewport de uma referência (um salão de metrô
subterrâneo abandonado: vigas de aço, telhado quebrado com feixes de sol na poeira, um vagão velho enferrujado com
grafite à direita, concreto molhado com poças, trilhos e dormentes, mato entre os trilhos, luz quente contra sombra fria).
A referência é uma imagem gerada: não dá para casar pixel a pixel; a cena reproduz a composição, os materiais, a luz e o
clima com o que a engine tem. Comparação lado a lado: `metro_comparacao.png` na pasta de notas da sessão.
- **Abrir**: `realengine --new-project "<Real Engine Projects>/MetroAbandonado" --template 2 --scene metro` (projeto de
  Primeira Pessoa). O nível vai para `Content/Maps/MetroAbandonado.rlevel`, mapa inicial do editor e do jogo, no Alto.
  A câmera do editor começa no enquadramento da referência; Play anda pelo salão.
- **Assets** (linhas `Metro` do `data/demo/manifest.tsv`, licenças em `LICENSES.txt`, todos CC0 1.0, baixados no build
  para a DemoContent em 1K):
  - ambientCG: Concrete042A (chão e paredes), Rust009 (vigas), Metal041B (trilhos; a caixa do vagão), PaintedMetal006
    (a pintura velha abaixo das janelas), Gravel022 (brita), CorrugatedSteel007A (telhas), GraffitiSet001 (atlas de 16
    grafites) e Leaking006 (escorridos); da DemoContent já existente: Planks023A (dormentes) e Rust004 (chassi).
  - Poly Haven: weed_plant_02, fern_02, shrub_03, shrub_04, dry_branches_medium_01, old_tyre, barrel_03,
    cardboard_box_01, trashbag. Ficaram de fora o aro enferrujado (vem com esqueleto) e a urtiga (8 mil triângulos
    para 20 cm). Não há trem CC0: o vagão é montado de peças.
  - O `fetch_demo_assets.cmake` passou a guardar também o `*_Opacity.jpg` dos zips (o recorte dos decals).
- **A cena** (o layout escrito com +y à esquerda da câmera e espelhado por `MT_Y`: na engine, como na Unreal, +Y é a
  direita):
  - Salão de 64 × 17 × 11,5 m de caixas World Aligned; pilares e tesouras de vigas I (`SM_MetroIBeam`, uma malha de
    perfil); plataforma à esquerda; parede do fundo com a boca do túnel em arco e uma luz quente lá dentro.
  - **Telhado**: duas aberturas com a treliça ainda no lugar e frestas compridas ao longo do salão, estreitas de lado. O
    sol alto vindo da esquerda (pitch −62°) atravessa a vista de lado, e os feixes aparecem de perfil, separados e
    inclinados para o trem. Com frestas atravessadas ou o sol de frente, os feixes se sobrepunham num borrão.
  - **Trilhos**: Spline Mesh com o perfil dos dois trilhos (`SM_MetroRails`), dormentes como Spline Instances
    (65 cm, um pouco tortos) e a brita outro Spline Mesh (`SM_MetroTrackBed`). A linha A curva para a esquerda até o
    túnel; a B, reta, leva o trem.
  - **Trem**: dois vagões de 18 m de peças (painéis, janelas recuadas de vidro sujo, portas duplas, frisos, teto em três
    águas, ventiladores, chassi, caixas, truques e rodas, cabine com para-brisa). Grafite (6 materiais do atlas) e
    escorridos de ferrugem como Decals.
  - **Chão**: concreto com Wet Surface e ~60 poças (Decals de película lisa e escura, borda quebrada por ruído).
  - **Mato**: ator Foliage pintado pela cena (onde o sol cai, entre e ao lado dos trilhos, na beira da plataforma e
    das paredes): o tufo de capim feito pela cena (`SM_MetroGrassTuft`, 22 folhas, raiz escura, ponta amarelada), ervas
    e samambaias; arbustos, galhos, pneus, tambores, caixas, sacos e pedaços de concreto à mão.
  - **Luz**: sol 110 000 lux quente, Sky Light 0,3 (o céu só pelas frestas), neblina volumétrica 0,008/m com
    anisotropia 0,8, exposição fixa (EV 9), vinheta, grão, contraste.
- **Na engine** (para os feixes):
  - Os froxels da neblina leem o sol nos **mapas de sombra virtuais**: 4 amostras espalhadas dentro de cada froxel (na
    profundidade e na área do pixel, giradas por froxel), a fração iluminada. Antes era um raio no centro do froxel:
    escada de froxels inteiros acesos ou apagados. Fora dos níveis, ou sem os mapas, continua o raio.
  - Por isso a neblina (`weather_record_fog`) passou para depois dos mapas de sombra do quadro (antes vinha com as
    nuvens). A barreira de antes dos mapas espera também o compute.
  - Integração com um filtro 3×3 em tenda por fatia, na memória compartilhada: tira a mancha do tamanho de um froxel
    que os 2 raios do céu por froxel deixavam onde o teto tem frestas.
  - `--game-view`: o viewport no Game View (G, sem ícones), para capturas.
- **Medidas** (RTX 3060, 2560×1417, a câmera da referência, Play ejetado sem ícones; outros agentes usavam a GPU ao
  mesmo tempo, então vale o mínimo da GPU, ~o custo sozinho):

| Preset | fps (média) | GPU mínima | CPU |
|---|---|---|---|
| Baixo | 388 | 2,23 ms (~450 fps) | 2,0 ms |
| Médio | 216 | 4,15 ms (~240 fps) | 2,4 ms |
| Alto | 157 | 5,42 ms (~185 fps) | 2,7 ms |
| Épico | 111 | 7,53 ms (~133 fps) | 2,9 ms |

  O layout final (a linha A mais à esquerda, a luz do túnel, menos capim) medido com outro editor aberto na GPU: mínimos
  2,36 / 4,46 / 5,80 / 8,24 ms (médias 225 / 184 / 124 / 64 fps, puxadas pela GPU dividida).
  No Alto (médias com a GPU dividida): malhas 2,6-2,9 ms, cache de superfície da GI 1,1 (orçamento fixo do preset),
  denoiser/filtro das sombras 0,44, pré-passe 0,4-0,5, froxels 0,38, tone map 0,36, sondas 0,26, GTAO 0,2. 2,4 mil
  instâncias, 1,2 M triângulos, 1 luz com sombra.
- **Mais perto da referência (2026-11-20, pedido do usuário)**: comparação nova em `metro_comparacao_v2.png` (pasta de
  notas).
  - **Câmera**: a 1,3 m, olhando 7° para cima, lente de 95°. Aparece o telhado com a treliça, como na referência.
  - **Sol**: 60° de altura, vindo um pouco mais na direção da câmera (yaw −118°). O feixe desce na lateral do trem.
    - Testado: o sol de frente (contraluz) apagava o trem e jogava a luz na plataforma; descartado.
  - **Gradação**: uma LUT `.cube` gerada pela cena (`mt_write_grade`, `Content/Demo/Metro/MetroGrade.cube`, 33³):
    curva em S suave, sombras para o ciano e menos saturadas, altas para o laranja.
  - **Exposição e GI**: EV 8,7 (era 9), GI Intensity 1,8. O lado esquerdo (plataforma, tambores) deixa de ser preto.
  - **Poças**: duas largas no leito do trilho A à frente. As do trilho são projetadas 30 cm, para pegar a brita e o
    topo dos dormentes; com 4 cm não apareciam.
  - **Feixes nas poças** (engine, `weather.glsl` `fog_segment`, `mesh.frag`): o raio do reflexo nítido (rugosidade
    < 0,25) atravessa a neblina volumétrica.
    - São 8 pontos lidos dos froxels: a luz espalhada da fatia pela diferença da luz integrada, a extinção pela da
      transmitância.
    - A fase do sol é girada da direção da câmera para a do raio (limitada a 8×).
    - Fora da vista ou além dos froxels, o ponto não soma.
  - **Testado e descartado**: neblina volumétrica até 35 m (fatias mais finas). O fundo do salão passava para a
    neblina analítica, sem sombra, e virava um borrão claro.
  - **Medidas** (1440p, com o Tlender aberto na GPU): Alto 160 fps, Épico 113 fps. Igual a antes (157/111).
  - **Ainda diferente**:
    - o trem de caixas, grande demais no quadro;
    - falta o túnel em arco no meio da vista;
    - as colunas rebitadas, o entulho e a hera;
    - o ar fora dos feixes mais limpo na referência;
    - as bordas dos feixes, limitadas pelos froxels de 128 × 72: dobrar custaria ~4× naquele passe.
- **O trem (2026-11-20, pedido do usuário)**: comparação em `metro_comparacao_v3.png`.
  - **Casca de cima**: uma malha de perfil (`SM_MetroCarRoof`, 18 m): laterais da janela para cima, quinas de 40 cm
    de raio e teto abaulado (18 cm de flecha), no lugar da coroa chata e dos dois caimentos de caixas.
    - As pontas são fechadas por placas por dentro.
    - As calhas ficam ao longo das quinas e os ventiladores sobre o abaulado.
  - **Cor**: o corpo inteiro em aço velho (Metal041B mais escuro e marrom), como o da referência. O PaintedMetal006
    verde saiu.
  - **Faixa**: vermelha desbotada sob as janelas (`M_MetroRedPaint`: ruído entre dois vermelhos, meio fosca), dando a
    volta na frente da cabine.
  - **Cabine na ponta de perto**: o 1º vagão tem cabine também na ponta que a câmera vê (`mt_car(..., cab_near)`):
    parede, para-brisas, porta, faróis, a faixa. Antes a ponta ficava aberta.
  - **Câmera**: recuada para x −850, y 310. O trem ocupa menos o quadro, como na referência.
  - **Medidas**: Alto 170 fps (GPU 5,7 ms), com o Tlender aberto. A casca de malha custa menos que as caixas de antes.
  - **Ainda diferente**:
    - o lado do trem pega o sol em cheio pelo buraco grande do telhado (claro demais; na referência fica quase todo na
      sombra);
    - as janelas sem cantos arredondados e sem o interior escuro;
    - sem rebites, amassados nem hera pendurada.
- **A luz no trem e as janelas (2026-11-20, pedido do usuário)**: comparação em `metro_comparacao_v4.png`.
  - **Tábuas e chapas** atravessando cada fresta do telhado a passos irregulares (45-150 cm de largura, 110-330 de
    vão, semente fixa).
    - A faixa comprida de luz de cada fresta virou vários feixes separados, como na referência.
    - O lado do trem passou a pegar o sol em manchas: antes era uma faixa acesa de ponta a ponta.
  - **Janelas**: o vidro recuado 13 cm (era 5), uma moldura escura de 6 cm em volta de cada uma e os cantos cortados a
    45° (o arredondado de longe).
  - **Medidas**: Alto 165 fps (GPU 5,9 ms), com o Tlender aberto.
- **O arco e os pilares (2026-11-20, pedido do usuário)**: comparação em `metro_comparacao_v5.png`.
  - **Parede transversal a ~32 m** da câmera (x 2350..2450) com dois arcos:
    - o do trilho A, de 6 m de vão e topo a 6,3 m;
    - o do trilho B, de 4,8 m e topo a 6,6 m, por onde o 2º vagão passa.
  - **Desenho do arco**: o topo de cada arco em 14 lâminas, cada uma tão larga quanto o círculo deixa, mais as impostas.
  - **Atrás**: o buraco HB do telhado ilumina o espaço visto pelos arcos.
  - **Passarela** ao longo da face, a 7,6 m: laje, viga de borda e guarda-corpo laranja velho, com montantes a cada
    1,5 m e dois corrimãos. É o "andar de cima" da referência.
  - **Pilares da plataforma** (até a parede): um caixão de chapas de 70 × 50 cm em volta do perfil I, cantoneiras
    salientes nos cantos, talas a cada 1,5 m e chapa de base. De longe lê como aço rebitado.
  - **Medidas**: Alto 170 fps (GPU 5,7 ms), com o Tlender aberto.
- **Entulho, hera e plantas (2026-11-20, pedido do usuário)**: comparação em `metro_comparacao_v6.png`.
  - **Entulho** (`mt_rubble`): 6 montes de concreto quebrado no primeiro plano à esquerda, na plataforma e ao longo
    dos trilhos.
    - Lajes de 50-90 cm e pedaços de 12-45 cm, girados ao acaso e mais altos no meio do monte.
    - Uma tábua solta a cada 8 pedaços.
  - **Hera** (`SM_MetroIvy`, feita pela cena): 14 fios de 1,5-4 m pendurados de uma linha de 1,2 m, com uma folha a
    cada 9-14 cm (quads de 7-12 cm, material do capim, dos dois lados).
    - Fica no alto dos pilares, sobre os arcos e na beira do teto do 1º vagão.
  - **Plantas**: o Foliage ganhou o capim seco amarelado.
    - O componente tem só 4 tipos (`FOLIAGE_TYPES`; pôr 6 escrevia fora do array).
    - Arbustos e galhos secos colocados à mão nos montes e entre os trilhos.
  - **Janela quebrada no alto da parede esquerda** (x −700..300, 7,2-10,8 m, com montantes e travessa).
    - O sol, vindo da esquerda, entra por ela e acende o entulho do primeiro plano, como na referência.
    - Antes esse canto ficava preto.
  - **Medidas**: Alto 159 fps (GPU 6,0 ms, CPU 3,5 ms; ~120 caixas a mais de entulho e montantes), com o Tlender
    aberto.
  - Testado e descartado: metade do mato e um terço das poças (−0,05 ms no mínimo: o mato não é o gargalo); sem os
    decals −0,36 ms, sem reflexo por raio −0,24 (ficaram: as poças e os grafites são a cena).
  - Os froxels: com o raio por froxel 0,65-0,79 ms; pelos mapas 0,38-0,50 (Épico, mesma cena). A tenda na integração
    custa +0,03-0,05 ms (0,02 → 0,05-0,08 com a GPU dividida).
- **Teste** `demo_metro` (área demos): cria o projeto, monta a cena no Alto, com validação: nível salvo, plantas
  pintadas, sem FAIL/VUID, > 95 fps.
- **Falta / limites**: os reflexos por raio não somam a neblina (as poças não espelham os feixes como na referência);
  o vagão é de caixas (sem chanfros nem rebites); no Alto a GPU fica em ~5,4 ms, acima dos 5 ms dos 200 fps (os custos
  fixos do preset, ver a tabela).
