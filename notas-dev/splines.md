# Splines e cabos (Q14, plano 0.43, lote 5, 2026-11-18)

A **Spline** não é um componente novo (o ECS está nos 64 componentes): o `CRail` cresceu no fim e virou a curva única da
engine (`COMP_SPLINE` = `COMP_RAIL`, `typedef CRail CSpline`). O campo `kind` diz o que ele é: **Rail** (trens, trilhos
desenhados por `vehicle_play.inc`), **Spline** (o Spline Component da Unreal) ou **Cable** (o Cable Component). Código:
`src/spline.c/.h` (a curva pura, re_core: o `ecs.c` usa os defaults), `src/spline_world.c` (peças, cabos, visões do
renderer, nós; re_render), `src/editor_spline.inc` (Details, Colocar Atores, malhas de exemplo, cena `splines`; incluído
pelo `editor.c`), `src/spline_test.c` (testes de CPU), `src/ui_tips_table_q14.c` (descrições), ramo `pad == 2` do
`shaders/skin.comp`.

## A curva (`spline.c`)
- Hermite cúbica entre os pontos; tipos da Unreal: **Curve** (tangente automática = ½(P₊₁ − P₋₁), pontas presas: é o
  Catmull-Rom uniforme que rios e trilhos amostravam), **Curve Custom Tangent** (arrive/leave por ponto), **Linear**
  (T₀ = T₁ = P₁ − P₀: reta com velocidade constante), **Constant** (degrau). Laço fechado, **roll** (graus, interpolado) e
  **escala** (2 valores, interpolada) por ponto; até 64 pontos (o limite do Rail).
- `SplineCurve` (~10 KB, na pilha ou estática; nada de heap por quadro): pontos e tangentes resolvidos num espaço (o do
  ator, ou o do mundo por `rows`: Hermite é invariante por afim, a curva no mundo é a do ator transformada) e a tabela de
  comprimento: 16 passos por segmento, cada passo integrado por Gauss-Legendre de 3 pontos de |H'|.
- Consultas: por **distância** (`spline_key_at_distance`: busca binária na tabela + 2 passos de Newton no comprimento
  exato do passo; o chute linear errava 0,5 cm numa reta de 10 m), por **chave de entrada** (ponto i = chave i), quadro
  (frente, direita, cima; `spline_frame`: cima = Z do ator, roll em volta da frente; o GLSL do skin.comp é a mesma conta),
  ponto mais próximo (amostras + Newton), comprimento.
- **Rios e trilhos**: `river_samples` (river.c) e `rail_build` (vehicle_sim.inc) avaliam pela `spline_eval_key`; o teste
  `spline_rail_river` compara com o Catmull-Rom antigo: 0,0007 cm no rio, 0 no trilho. Um Rail agora aceita tipos e
  tangentes nos Details (o hash das peças só muda quando algum ponto tem tipo: os trilhos antigos não são refeitos).
- **Níveis antigos**: as linhas do `CRail` gravadas no tamanho antigo (até `built`) leem os defaults novos e o
  `component_upgrade` do `ecs.c` põe `kind = Rail` (o default de um componente novo é Spline). Teste: um blob reescrito
  como o motor antigo gravava carrega um Rail com seus pontos, escala 1 e tipo Curve.

## Spline Mesh (na GPU, sem copiar a malha)
- Cada peça (um segmento, ou cada **Mesh Length** da curva dividida por igual) é uma entidade filha (`CAttach`
  `ATTACH_SPLINE`, nó `SPLINE_NODE_MESH | i`, Movable) com a malha. O `render.c` acha a visão da peça
  (`spline_mesh_view`: P₀, T₀, P₁, T₁ no mundo, roll, escala, cima, eixo frontal, x mínimo e 1/comprimento da malha) e a
  manda ao `skin_bind`, que escreve 12 vec4 e um job no **skin.comp** (`pad = 2`): o vértice no eixo frontal vira o t da
  Hermite, o corte transversal vai no quadro da curva vezes a escala; a normal pela cofatora do Jacobiano. Os vértices
  dobrados ficam no buffer dos skins: sombra, BLAS (raios), vetores de movimento e GI como qualquer skin.
- **Só quando muda**: o job sai quando a versão (hash da visão e das linhas da instância) muda; uma estrada parada não
  custa nada por quadro na GPU depois da primeira dobra.
- **Transform da peça = a corda**: frente de P₀ a P₁ (escala = corda / comprimento da malha), lados do quadro no meio da
  curva. Assim a esfera do culling e os texels do cache de superfície (que usam as linhas da instância e a malha não
  dobrada) ficam sobre a peça; numa curva forte a esfera é aumentada até cobrir a dobra (o desenho continua exato: o
  skin.comp escreve pelo inverso das linhas). As peças levam `CAST_SHADOW_NO_OCCLUDER` (o AO de contato usaria a caixa
  das linhas).
- **Espaço dos skins**: muitas peças pequenas pediam uma recarga das malhas por peça (dezenas seguidas; numa delas a GPU
  caiu). Agora `skin_bind` soma o que faltou no quadro e `skin_room_grow` recarrega uma vez (o dobro, no mínimo).
- Malhas de exemplo feitas na hora em `/Content/Meshes`: **SM_SplineRoad** (4 m, meio-fio, 16 anéis em 2 m) e
  **SM_SplinePipe** (raio 20 cm, 16 lados, 16 anéis): as formas básicas têm 2 anéis no comprimento (dobram só nas pontas).

## Instâncias ao longo da curva
Filhas `SPLINE_NODE_INST | i` com a mobilidade do ator: a cada **Spacing** a partir do **Start Offset**; semente
(hash determinístico) para o deslocamento lateral, o giro e a escala (mín..máx) vezes a **Instance Scale** (X, Y, Z: um
mourão fino e alto a partir do cilindro), **Up Offset** (de pé sobre a curva); **Align** Em Pé (yaw ao longo), Ao Longo
(o quadro com roll) ou Como o Ator; **Instances Collide** as põe nas caixas de colisão do jogo (`bp_vm.c` aceita
`ATTACH_SPLINE` de instância). Refeitas só quando o hash da spline (campos, pontos, transform) muda, com o contador de
mudanças do mundo como portão (o mesmo do protótipo): um quadro sem mudança não percorre nada.

## Cabo (Cable Component, XPBD)
- `CableSim`: até 65 partículas; por subpasso: Verlet (velocidade, amortecimento, gravidade, arrasto para a velocidade do
  vento), restrições de distância XPBD (λ por restrição; Stiffness 1 = complacência 0), flexão como distância i↔i+2 só
  de empurrar (Bend Stiffness), depois colisão com atrito. Pontas presas (massa inversa 0) andam até o alvo ao longo dos
  subpassos (um braço girando não dá tranco).
- **Pontas**: ator por nome (achado e guardado) e um **socket ou osso** (`anim_socket_world`, Q10) com deslocamento no
  espaço dele; sem ator: este ator; **Free End**. `cable_attach`/`cable_detach` (nós) mudam isso no jogo.
- **Colisão**: landscape (`landscape_height`), caixas orientadas das malhas perto (estáticas, corpos rígidos, peças de
  protótipo; planos viram uma laje de 50 cm), cápsulas dos Characters e, jogando, as caixas do jogo (`bp_world_boxes`). O
  ponto sai pela face por onde entrou (a posição antes do subpasso), nunca atravessa a caixa. As pontas e o próprio ator
  não empurram o cabo (um poste não chuta o fio preso nele).
- **Desenho**: uma peça por segmento (cilindro em pé, eixo Z: Tubo; plano: Fita), dobrada pelo mesmo skin.comp entre
  duas partículas com tangentes de Catmull-Rom: as juntas dividem o quadro, o tubo é contínuo. Parado (< 0,02 cm), não
  dobra de novo.
- Simula no editor (pendura enquanto se edita) e no jogo; `spline_begin_play/end_play` recomeçam do repouso (uma
  parábola do comprimento: `L ≈ D + 8h²/3D`).

## Editor
- **Colocar Atores > Mundo > Splines**: Spline, Spline Mesh (a estrada), Spline Instances (cerca), Cable.
- **Viewport**: a curva desenhada pela avaliação comum (12 passos por segmento), pontos arrastáveis (Shift: vertical;
  setas por eixo; Alt: ponto novo; botão direito: adicionar/excluir no meio da curva), **alças de tangente** ciano no
  ponto selecionado (arrastar vira Curve Custom Tangent, as duas iguais).
- **Details**: Spline (tipo, laço, comprimento, pontos, Editar Ponto: tipo, tangentes, roll, escala), Spline Mesh,
  Instâncias ao Longo da Spline, Cable (pontas com lista de atores, comprimento com "Ajustar às Pontas", física, vento,
  desenho, material, estatística). **Debug Viewer > Splines and Cables**.

## Blueprint (23 nós, bloco Q14 no fim do enum)
Get Spline Length, Get Location / Direction / Rotation / Transform at Distance Along Spline, Get Location at Spline Input
Key, Get Distance Along Spline at Spline Input Key, Find Input Key / Location Closest to World Location, Get Number of
Spline Points, Get Location at Spline Point, Add / Set Location at / Remove Spline Point, Clear Spline Points, Set Closed
Loop, Set Spline Point Type, Set Attach End To, Set Attach Start To, Detach Cable End, Set / Get Cable Length, Add Cable
Impulse. Os que mudam a curva valem no Construction Script.

## Medidas (RTX 3060, 1440p)
- Curva: construir 6 pontos fechada 1,8 µs; consulta por distância ~110 ns (com Newton).
- Cabo: um passo de 32 segmentos (4 subpassos × 8 iterações) 23-24 µs na CPU. Cena `splines` (4 cabos, 112 partículas):
  atualização 0,14 ms de CPU por quadro (cabos ~0,1 ms), pico 0,4 ms.
- Spline Mesh: 21 peças (estrada e cano) dobradas uma vez; os 88 segmentos dos cabos em movimento são dobrados a cada
  quadro: skins 0,21 ms de GPU com o refit das BLAS (108 jobs; parados, 0).
- Cena `splines` com validação em Play: 329 fps (Médio). Teste `splines_scene` cobra 200 fps, nada atravessa a caixa,
  ponta no braço girando a 0,00 cm, cabo entre postes com barriga e sem esticar.
- Cenas sem splines: ver "Sem regressão" abaixo.

## Sem regressão
Base 827f75c compilada à parte, alternando base e Q14, 3 s cada, `--gpu-passes` (a GPU dividida com outra sessão em
algumas rodadas: essas ficaram de fora):
- `--scene rain --scalability 3`: base 188 / 185 / 186 fps (GPU 5,17-5,30 ms), Q14 191 / 193 / 186 fps (GPU 5,08-5,27 ms).
- `--scene house`: base 481 / 473 / 388 fps, Q14 483 / 506 / 387 fps (a última dupla com outra sessão na GPU).
- O que roda sem splines: `spline_update` percorre os arquétipos (nenhum `COMP_SPLINE` fora dos Rails: lista vazia) e só
  limpa peças órfãs quando o mundo mudou; no renderer, uma leitura de `CAttach` por entidade dinâmica não-skin. O ramo
  novo do skin.comp só roda com jobs de spline.

## Correções de passagem
- **Debug Viewer**: `DV_MAX_LISTS` era 16 e 25 listas se registram: as do fim (HUD, Horror Kit, Frame Budget, a das
  splines) sumiam em silêncio. Agora 32.
- `dock_targets` falha às vezes com janelas de outras sessões na tela (o clique do teste não começa o arraste); a mesma
  build passa 4/4 com a tela livre. Não é deste item.

## Pendências
- Os jobs do skin.comp são um dispatch cada: juntar os de um quadro num dispatch só baixaria o custo dos cabos em
  movimento (~2 µs por segmento hoje).
- Spline Component dentro de Blueprints (SCS): o nó SCS tem 352 bytes de dados (BP_SCS_DATA), o CSpline ~3,6 KB; hoje a spline é um ator
  (ou o componente nos Details de qualquer ator) e os Blueprints a referenciam.
- O cabo não puxa os corpos onde está preso (como o da Unreal).
