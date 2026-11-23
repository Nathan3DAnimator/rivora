# Física (`src/physics.c`)

O "Simulate Physics" da Unreal: corpos rígidos que caem, colidem, empilham, rolam, dormem e quebram.

## Como usar
- **Detalhes > Adicionar > Física** num ator com malha: Simular Física, Massa (0 = automática, 100 kg/m³), Gravidade,
  Amortecimento linear/angular, Atrito, Restituição, **Fratura** (velocidade do impacto que quebra, pedaços por eixo
  2–4) e Velocidade Inicial. Ligar a simulação deixa o ator Movable (como a Unreal exige).
- Blueprints (categoria Physics): **Add Impulse** (kg·cm/s; "Vel Change" soma a velocidade direto), **Set Simulate
  Physics**, **Add Radial Impulse** (explosão: cm/s no centro, zero na borda), **Get Physics Linear Velocity**.
- Debug Viewer: lista **Physics** (tempo do passo; cada corpo com a parte do tempo pelos contatos que teve).
- (Q8) **Usar CCD**, **Material Físico** (asset `.rphysmat`), **Velocidade Angular Máxima**; Project Settings > Physics >
  **Solver Substeps**; juntas **Cone e Torção** e **6DOF**; 20 nós novos. Ver "Corpos rígidos profissionais" abaixo.

## Como funciona
- Forma: **Details > Física > Forma de Colisão** (ver "Formas de colisão" abaixo). Colisores estáticos:
  as outras malhas do nível (grade de 4 m, as grandes numa lista à parte), reunidos ao começar e a cada 0,25 s num
  nível pequeno (portas e plataformas de Blueprint). Characters entram como paredes que andam (empurram as caixas).
  Componentes de Blueprint com malha também colidem (menos os de um Character).
- (Q8: o solver, a broadphase, o sono e as juntas abaixo foram trocados; ver "Corpos rígidos profissionais".)
- Passo fixo de 120 Hz (até 4 por quadro). Contatos: caixa×caixa pelo teste dos 15 eixos separadores; eixo de face →
  a face incidente recortada contra a de referência (até 8 pontos: pilhas assentam planas); eixo de aresta → pontos mais
  próximos das arestas. Esfera×caixa pelo ponto mais próximo. **Contatos especulativos** (margem de 1 cm): uma caixa
  parada mantém os 4 cantos e não balança.
- Solver: impulsos sequenciais (6 iterações), normal com restituição (acima de 1,2 m/s, só no contato que fecha no
  passo) e empurrão suave para fora da sobreposição (Baumgarte 0,25, folga 0,5 cm, no máximo 3 m/s: o Max
  Depenetration Velocity da Unreal), atrito de Coulomb em duas direções. **Warm start**: cada contato começa do impulso
  com que terminou no passo anterior (achado pelo par e pelo ponto no primeiro corpo; a normal e o atrito guardados no
  mundo: se a normal girou mais de 25° não reaproveita, o atrito é projetado nas tangentes novas). Quem acorda é
  decidido antes das massas efetivas (um corpo acordado no meio da passada era parede para os contatos de antes).
- Sono: parado (< 10 cm/s, < 0,2 rad/s) ou tremendo no lugar (1,5 cm e 2°) por 0,5 s. Um corpo dormindo só acorda com
  um impulso de Blueprint ou um choque (> 25 cm/s) de um corpo que se move de verdade — o monte não se acorda sozinho.
  Bolas, cilindros e cápsulas no chão têm resistência ao rolamento (exponencial 1,2/s e mais uma parte constante de
  2 rad/s²: uma rolagem lenta termina).
- Sem passo de 120 Hz no quadro (fps alto) nada é reunido nem escrito de volta: a física não custa nesses quadros.
- Fratura: depois do passo, o corpo que bateu acima da velocidade vira n³ cubos (corpos, sem fratura) com a velocidade
  daquela parte (v + ω×r), um empurrão para fora e um pouco de acaso; até 512 pedaços por quadro. No PIE o snapshot
  traz o original de volta ao parar.
- Nada aloca por quadro (reservas de mem.h); sem corpos no nível o custo é a varredura dos arquétipos.

## Medidas (RTX 3060, 1440p, Release)
| Cena | Resultado |
|---|---|
| `--scene physics` (pirâmide 15, torre 6, 3 bolas na rampa, caixa que quebra) | 51 corpos, pico 0,38 ms; 391 fps |
| `--scene physics_stress` (1000 caixas/bolas caindo num monte) | ~2,1 ms por passo de 120 Hz com 3000 contatos (achar 0,8, resolver 1,1, preparar 0,2); pico 5,8 ms/quadro; dorme todo em ~12 s; depois 0,09 ms |
| Iterações 10 → 6 (com warm start) | resolver 1,6 → 1,1 ms, assenta mais rápido |
| Formas de colisão (2026-11-08), `physics_stress` antes → depois | 2,37 → 2,22–2,32 ms por passo com o monte ativo (média de 3 rodadas cada; o caminho caixa-caixa e esfera-caixa é o mesmo); assenta em 5,9–6,3 → 5,4–7,5 s |
| `physics_stress_shapes` (os mesmos 1000, um quarto cada: caixa, cilindro, cone, esfera) | 4,6–5,1 ms por passo (achar 2,1–3,0: ~2400 GJK por passo, EPA raro: 12 mil no teste inteiro); assenta em ~6–9 s |
Testes: `physics` (tudo no chão, a caixa quebrou, ≥ 300 fps), `physics_stress` (o monte dorme), `physics_stress_shapes`
(o monte misto dorme, ninguém atravessa o chão) e `physics_shapes` (as formas, abaixo).

## Limites conhecidos / próximos passos
- (Feito em 2026-11-08: cilindro, cone, cápsula, casco convexo e malha complexa; ver "Formas de colisão".)
- (Feito em 2026-11-18, Q8: ilhas, threads, sub-passos, CCD, 6DOF; ver "Corpos rígidos profissionais".)
- Fratura em cubos (Voronoi com malhas geradas depois). Juntas: ver "Physics Constraint".
- (Feito em 2026-11-08: Characters sobem, ficam em cima e são levados; ver "Character contra física".)

## Character contra física (2026-11-08, plano de melhorias D3)
As caixas de colisão do Play eram tiradas uma vez no começo: um corpo com física que caía ou era empurrado continuava
"colidindo" onde estava, e uma plataforma Movable não levava ninguém.
- `bp_vm.c`: as caixas de atores Movable, com física ou de componentes de colisão são lembradas (`BoxTrack`: entidade e
  forma local). `track_boxes`, a cada quadro antes dos Characters, relê a transformação; a que se moveu sai da grade
  (as células eram as do começo) para a lista grande, varrida inteira, e guarda o quanto andou (`Box.d`).
- **Plataformas**: o Character guarda a caixa em que pisou (`CCharacter.ground_box`); se ela andou, ele anda junto antes
  de se mover.
- **Empurrar**: andando contra um corpo com Simular Física, a velocidade do corpo nessa direção vai até a do Character
  (toda até 80 kg, proporcional acima: massa 0 = 100 kg/m³ da caixa). Degraus de até 45 cm já existiam.
- **Colisão Ligada** (Details > Static Mesh; flag `TRANSFORM_NO_COLLISION`): desligada, a malha não entra nas caixas
  (o NoCollision da Unreal). O ator de tipos do `bp-test` (escala 42 no fim) ficou sem colisão.
- Teste `char_physics` (`--char-physics-test`): sobe o degrau de 30 cm (z 118), empurra a caixa de 20 kg de 600 para
  ~1110, a plataforma anda 370 cm e o Character em cima também. 455 fps com validação.
- (Feito em 2026-11-09: atores criados no Play entram nas caixas e os destruídos saem; a plataforma que gira gira quem
  está em cima. Ver "Atores criados no Play e plataformas que giram".) A forma continua sendo a caixa alinhada.

## Physics Constraint (2026-11-08, plano de melhorias D2d)
- Componente `COMP_CONSTRAINT` (Details > Adicionar > Physics Constraint): Ator 1 e Ator 2 (vazio = o mundo), Junta
  Fixa / Dobradiça / Esfera / Prismática, limite (graus ou cm, em X), motor (velocidade, força), Força de Quebra,
  Desligar Colisão. A junta é a posição do ator dela; o X dele é o eixo. Âncoras locais e rotação relativa de repouso
  guardadas na primeira passada (campos do run, zero no nível).
- Solver: linhas de impulso sequencial (`JRow`) ao lado dos contatos, montadas a cada passo (`prepare_joints`): 3
  lineares nas âncoras (a prismática: 2 através de X + limite/motor em X), angulares pela rotação desde o começo
  (dobradiça: 2 através de X + limite/motor em X; fixa e prismática: 3). Correção de 30% da deriva por passo, duas
  passadas de juntas por iteração. Um lado acordado acorda o outro; corpos presos não colidem entre si.
- Nós: Break Constraint, Set Constraint Motor (Target = o ator da junta).
- Teste `constraints` (`--constraint-test`): pêndulo (braço 100,0 cm), porta com limite de 60° (parou em -30° a -55°),
  par fixo caindo (56,6 cm, no chão), trilho com motor parando em 150 cm; deriva máxima das âncoras 2,1 cm; 447 fps.

## Formas de colisão (2026-11-08, plano de melhorias D2a-b / B2a-b)
`src/physics_shapes.c` (formas, GJK, EPA, manifold, cascos, BVH) e `physics.c` (qual forma, os pares, o cache).
- **Details > Física > Forma de Colisão** (`CRigidBody.collision_shape`, campos novos no fim: um nível antigo abre como
  Automática): **Automática** = cubo/plano caixa, esfera esfera, cilindro um **cilindro exato** (oval: casco), cone e
  malhas importadas o **casco convexo**; ou Caixa, Esfera, **Cápsula** (no eixo mais comprido, raio dos outros dois),
  **Convexo**. Um casco que é a própria caixa dos limites (8 vértices, 6 faces) usa o atalho caixa-caixa.
- **Usar Malha Complexa** (`complex_collision`, aparece com Simular desligado): os triângulos da malha numa BVH (mediana
  no eixo mais longo, 4 por folha) para o cenário; convexo × malha triângulo a triângulo dos que tocam a caixa do corpo
  (com o deslocamento do passo), uma face só (o triângulo de que o centro está atrás não conta). Arestas internas: um
  triângulo alinhado com a normal mas sem nada sobre ele não dá contato (é do vizinho; a bola quicava de lado na
  diagonal do degrau).
- **Casco**: os pontos extremos da malha em 154 direções (26 da caixa + 128 de Fibonacci, AVX2 8 por vez) são vértices
  do casco; casco incremental (as faces visíveis saem, o horizonte fecha no ponto novo); mais de 64 vértices: menos
  direções sobre esses pontos até caber; triângulos coplanares viram faces (polígonos ordenados); volume, centro de
  massa e tensor de inércia por tetraedros (o corpo gira em torno do centro de massa, inércia cheia 3×3). Feito uma vez
  por malha (cache, refeito quando as malhas recarregam): o dos corpos ao reuni-los, o dos estáticos **só quando um corpo
  chega perto** (até lá a caixa dos limites; a maioria do cenário nunca é tocada). Medida: 100 mil pontos → 53
  vértices, 99 faces em **3,0–3,5 ms**; o prisma e o cone de teste 0,05 ms. Não mexe no `.rmesh`.
- **Detecção**: caixa×caixa (SAT de 15 eixos), esfera×caixa e esfera×esfera continuam os testes próprios. O resto:
  **GJK em double** (com o chão de 40 m o float perdia o ponto mais próximo e dizia "separados" com o corpo 10 cm
  dentro) sobre núcleos com **pele** (caixa, cilindro e casco encolhidos 1 cm — no máximo ¼ da menor medida — e
  arredondados por ela, como o convex radius do Jolt): em repouso os núcleos ficam separados e só o GJK roda; **EPA**
  só num choque mais fundo que as peles, medido depois ao longo da própria normal e conferido contra os eixos das formas
  (fica o de menor sobreposição: um EPA perdido não manda nada para baixo do chão). **Manifold**: as features de apoio
  das duas (face, aresta ou ponto: tampa do cilindro em 16 lados, lateral em segmento, face do casco pela normal),
  a mais alinhada com a normal (≥ 0,9) é a referência e a outra é recortada nos lados dela; até 4 pontos (o mais
  fundo, o mais longe dele, o triângulo maior, o outro lado). Dois segmentos paralelos (cápsulas, laterais): a
  sobreposição deles.
- **Contatos especulativos** nos pares do GJK: margem de 1 cm + velocidade relativa × passo (até 1 m): um corpo rápido é
  pego antes de entrar.
- Problemas achados no caminho: o suporte do cilindro com a direção exatamente no eixo usava o ruído do resto como raio
  (às vezes ao longo do próprio eixo: o suporte voltava o centro); o warm start reaproveitava o impulso com a normal
  girada (energia no monte de cones).
- Teste `physics_shapes` (`--new-project ... --scene physics_shapes --play`; as malhas de teste vão para o projeto):
  cilindro e cone de pé (z 30,5), cone deitado com o eixo a −0,447 (sen 26,6°, ponta e borda no chão), cápsula deitada
  (z 15,5), cilindro que desce a rampa e rola 6 m até parar deitado, 3 prismas octogonais "importados" empilhados (15,4 /
  45,3 / 75,3), 4 caixas e 4 bolas numa escada "importada" (não convexa, Malha Complexa) exatamente nos degraus (35, 60,
  85, 110); casco de 100 mil pontos medido. 0,07 ms por passo; > 200 fps.
- Limites: a malha complexa é só para o que não se move; o casco é um (sem decomposição convexa à la V-HACD); os
  Characters continuam caixas no `bp_vm.c`; GJK numa thread (as ilhas em paralelo são o B2c).

## Atores criados no Play e plataformas que giram (2026-11-09, sobras da Fase 1)
- **Criados**: `bp_spawn` (e quem mais criar atores no Play) chama `bp_box_spawned`; no tick seguinte uma passada pelos
  arquétipos que podem bloquear pega os do ator e dos componentes dele (busca binária no conjunto dos criados) e os
  põe numa **grade própria** (cadeias por célula: `xhead`, `xref`), sem refazer a do nível. Os que se movem entram na
  lista de rastreio como os do nível. `collide`, `bp_trace_boxes` e `bp_world_boxes` leem as duas grades.
- **Destruídos**: Destroy Actor, a morte da Vida e `bp_destroy_actor` marcam `boxes_sweep`; no tick seguinte as caixas
  cuja entidade ou dono (`Box.owner`, o ator de um componente) morreu ficam vazias (antes refazia tudo). O traço contra
  uma caixa vazia agora a ignora (os slabs invertidos davam "bateu em 0").
- **Giro**: `track_boxes` guarda o lugar e o yaw da entidade (`BoxTrack.loc/yaw`); o deslocamento é o da entidade e o
  giro o do yaw. O Character em cima gira em volta do pivô com ele, e a rotação dele e a de controle (a câmera) somam o
  giro, como o based movement da Unreal.
- **Teste `char_physics`** (cresceu): parede criada a 0,4 s na frente de um terceiro Character (para em x 441, a face 475
  menos o raio 34), destruída a 2,2 s (ele passa: 971); plataforma 3 × 3 m girando 45°/s com o Character a 80 cm do
  centro: depois de 166° está a 80,0 cm, 166° em volta, yaw e controle com 0,2° de erro. 3000 cubos pequenos dão volume
  à grade. A medida de refazer tudo vai antes de as plataformas andarem (refazer zera os movimentos: um quadro de
  transporte perdido, 11 cm com quadros de 0,1 s).
- **Medidas** (3011 caixas): adicionar a parede 0,08–0,12 ms (inclui limpar as cabeças da grade nova, 256 KB, uma vez
  por Play), varrer 0,011–0,017 ms, refazer tudo 0,19–0,24 ms. 445 fps.

## Character nascendo dentro do chão (2026-11-10, "o player spawna fora do chão", FirstPersonMap)
- **O defeito:** um Plane com escala uniforme (208 no MyProject) tinha a espessura dos limites (±0,5) escalada junto:
  uma caixa de colisão de ±104 cm em volta dos pés. No primeiro tick o Character, parado dentro dela, era empurrado
  pelo eixo X (velocidade 0 ia para o lado +) até a borda do chão (x 10 441) e caía.
- **Agora:** malha plana (meia espessura local <= 0,5) fica com 1 cm a qualquer escala (`mesh_box`, `bp_vm.c`); e quem
  está com os pés afundados no topo de uma caixa (até meia altura) sem andar naquele eixo **sobe** para cima dela; mais
  fundo que isso é parede e segue a regra antiga (sair pelo menor caminho empurrava para trás o jogador que o teste
  `world_partition` teleporta 400 m/s pela cidade).
- Teste `char_physics`: Character sobre um Plane de escala 30 x 30 x 30 fica onde nasceu (z 289).

## Corpos rígidos profissionais (2026-11-18, plano 0.43 Q8)
Pedido de 17/11: "o sistema de física geral como rigid body mais completo, mais preciso e otimizado, para simular uma
cena bem pesada em tempo real". `physics.c` reescrito por dentro (as formas, o GJK/EPA, os estáticos, a fratura e a API
antiga ficaram); `physmat.c` / `physmat_editor.c` (Material Físico); testes em `phys_test.c` e `phys_joints_test.c`.

**Corpos persistentes.** Os corpos ficam de um quadro para o outro (um mapa entidade → corpo); só são reunidos de novo
quando corpos entram ou saem (um arquétipo que mudou de linhas só troca os ponteiros). Cada quadro pega o que mudou por
fora: velocidade (Add Impulse...), teleporte (a transformação diferente da escrita: refaz a pose), massa, forma, material.
A escrita de volta é em paralelo e **não entra no log de mudanças do ECS** para atores Movable (o render já reenvia os
Movable todo quadro; 20 mil corpos estouravam o anel de 4096 e faziam o render, o cache de GI e o Organizador recomeçar
do zero a cada quadro).

**Solver: o "soft step" do Box2D v3 em 3D** (TGS soft; Catto, "Solver2D", 2024: o melhor em pilhas entre PGS, TGS e
XPBD nos testes dele, e o que mediu melhor aqui: o antigo deixava a pirâmide de 1035 caixas andar 85 cm).
- Cada passo de 120 Hz: `solver_substeps` sub-passos (padrão 4 = 480 Hz), cada um integra velocidades, aquece (warm
  start), resolve com contatos **macios** (mola de 60 Hz com 4 sub-passos — ⅛ da taxa: ¼ fazia uma torre alta tremer —,
  amortecimento 10, os estáticos com o dobro), integra posições e **relaxa** sem o bias. Restituição depois (acima de
  1 m/s). A separação de cada ponto é atualizada pelo movimento dos corpos no sub-passo (o narrowphase roda uma vez por
  passo). Afundar de uma pilha: N·g/(2ω²) por andar (30 Hz: 14 cm na pirâmide; 60 Hz: 3,2; 90 Hz: 1,2).
- **Atrito de patch**: no meio dos pontos do manifold, duas direções e a **torção** em volta da normal (um disco de
  μ·Σjn), com **âncoras de atrito estático** (uma mola macia puxa as âncoras de volta enquanto segura; escorregou mais de
  5 mm: âncoras novas). Os pontos normais em **Gauss-Seidel simétrico** (ida no solve, volta no relax, o sentido
  trocando a cada sub-passo): a ordem fixa empurrava sempre um lado e a caixa do topo da pirâmide andava 8,6 cm em y.
- **Paralelo por coloração do grafo**: cada contato ganha a primeira cor que nenhum dos dois corpos acordados tem
  (máscara de 32 bits por corpo; 24 cores e uma de sobra resolvida por uma thread); uma cor inteira em paralelo. Um
  `re_parallel_for` por passo roda **todos os estágios** (contadores atômicos por estágio, cada worker espera o fim do
  estágio; quem chega atrasado acha pronto; uma thread sozinha faz tudo em ordem). Só as cores usadas viram estágio.
- Juntas: as mesmas linhas macias, refeitas das poses a cada sub-passo, 4 passadas no solve (2 no relax).

**Broadphase.** Acordados numa **grade hash 3D** refeita todo passo (célula = 4× o tamanho médio; um corpo em mais de
64 células vai para uma lista à parte); dormindo noutra grade, refeita só quando alguém dorme ou acorda; o cenário na
grade 2D de antes. Os pares contados e depois preenchidos bloco a bloco em paralelo (sempre na mesma ordem:
determinístico); um par uma vez só (na célula do maior dos cantos de baixo). SAP num eixo mediu mal num mundo espalhado
em XY (~170 vizinhos por corpo na cena de 20 mil); a BVH incremental tinha inserção serial cara com tudo caindo.
Na GPU: não feito.

**Narrowphase** em paralelo (cada par escreve as suas vagas de manifold; até 4 pontos: o mais fundo, o mais longe, o
maior triângulo e o outro lado; triângulos de uma malha virados igual juntam-se num só). Especulativo até 50 cm pela
velocidade. O casco dos estáticos continua feito quando um corpo chega perto, numa passada serial antes.

**CCD.** Corpo com **Usar CCD** (bala: contra tudo) ou mais rápido que meia espessura por passo (contra o cenário, como
o Box2D) é varrido do começo ao fim do passo por **avanço conservador** (distância do GJK, `phys_distance`; giro pela
extensão) contra os pares que a broadphase achou (com o deslocamento do passo), até triângulos de malha complexa; para no
primeiro toque (o contato especulativo do passo seguinte segura).

**Sono por ilhas.** Union-find dos acordados ligados por contatos que tocam e por juntas; ilha parada (< 8 cm/s contando
o giro na borda) por 0,5 s dorme inteira, num **anel** (`gnext`, e o grupo em `CRigidBody.sleep_group`: o anel volta
quando os corpos são reunidos de novo). Encostar num dormindo acorda a ilha dele no passo seguinte; um Blueprint que
empurra acorda a ilha. Só roda quando algum corpo já pode dormir (e a cada 8 passos para a contagem).

**Juntas completas** (`CConstraint`: campos novos no fim; um nível antigo abre igual). A junta é o quadro do ator dela
guardado no espaço de cada corpo (`fa`, `fb`). Linear X/Y/Z no quadro do corpo 1; ângulos do corpo 2 visto do corpo 1
divididos em **torção** (X) e **balanço** (Swing 1 em Z, Swing 2 em Y). Cada eixo: **Travado** (mola de 60 Hz),
**Limitado** (um lado, especulativo dentro: nunca passa; **mole**: uma mola de Hz e amortecimento depois do limite),
**Livre**. Dois balanços limitados: **cone elíptico**. **Drives** (Unreal's Linear/Angular Drive, modo Acceleration: sem
massa): posição e/ou velocidade, Hz e amortecimento, força máxima; o angular é SLERP; medem o corpo 1 no quadro do
corpo 2 (o mundo). **Motores** antigos (dobradiça, trilho): fora quando estão no limite para o lado que empurram (um motor
sem limite de força contra o limite fazia o trilho subir 17 cm). Quebra por **força** e por **torque**. Junta com o ponto
travado: as linhas angulares usam a inércia **em volta da âncora** (eixos paralelos; o corpo gira em volta do pivô): sem
isso o cone de um pêndulo passava 8° do limite mesmo com 32 passadas.

**Material Físico** (`.rphysmat`, JSON): atrito, restituição, densidade (g/cm³: a massa quando Massa é 0), combinação
(Average, Min, Multiply, Max; vale o modo mais alto do par, como a Unreal). 8 predefinições (Content Browser > Adicionar
> Física). Sem material: as regras de antes (atrito pela média geométrica, restituição pelo maior).

**Blueprint** (20 nós; scripts pela API): Add Force, Add Torque in Degrees, Add Angular Impulse in Degrees, Add Impulse at
Location, Set Physics Linear/Angular Velocity, Get Physics Angular Velocity in Degrees, Set Mass Override in Kg, Get
Mass, Wake Rigid Body, Put Rigid Body to Sleep, Rigid Body Is Awake, Set Enable Gravity, Set Physical Material, Set Use
CCD; Set Linear Drive, Set Angular Drive, Set Constraint Limits, Set Constraint Axis Motion, Get Constraint Force.

**Medidas** (i7-11700F 8 núcleos/16 threads, RTX 3060, 2560×1417, Release; antes = main df71400):
| Cena | Antes | Depois |
|---|---|---|
| `phys_stack` (pirâmide 2D de 1035 caixas, base 45, sono ligado) | andou 85 cm, 94 cm/s depois de 3 s, 7,7 ms/passo, 31 fps | parada (0,00 cm depois de assentar), as 1035 dormem, afundou 3,2 cm; 4,3 ms/passo acordada |
| `phys_stack_awake` (sem sono, 6 sub-passos) | — | 0,14 cm, ≤ 0,8 cm/s, afundou 1,2 cm; 8,4 ms/passo |
| Pirâmide sem sono, 4 sub-passos (sem âncoras / com patch e GS simétrico) | — | 8,6 cm / 0,8 cm (a caixa do topo, em y) |
| `phys_ccd` (bolas de 2 cm a 50–1000 m/s, caixas de 10 cm, parede de 2 cm, bloco de 2 t) | as 8 atravessam | as 8 param; 523 fps |
| `phys_joints` (10 checagens) | — | todas; âncoras ≤ 0,47 cm |
| `constraints` (o antigo) | deriva 2,1 cm | 1,05 cm (o pêndulo agora guarda a energia: v² = 2gh) |
| `phys_heavy` (20 mil: caixas, bolas, cápsulas, cilindros; duas explosões) | 44,4 ms/passo (narrow 29, solver 12), 5 fps | 16,7–19,2 ms/passo, 11–14 fps |
| `physics_stress` (1000) | 2,2–2,3 ms/passo, assenta 5,4–7,5 s | 2,7–2,8 ms/passo (4 sub-passos, mais que as 6 iterações antes), assenta 7,4–8,3 s |
| `physics` | assenta 3,2 s | assenta 0,6 s |
`phys_heavy` por fase (ms por passo, quase tudo acordado): corpos 0,4; broadphase 2,9–3,3 (grades 1,3 serial, pares
1,8); narrowphase 2,5; cores 1,1–1,5 (serial); contatos 1,1–1,3; **solver 5,8–8,1**; finalizar 0,4; CCD 0,4; cache 0,4;
ilhas 0,5. A GPU nessa cena: 15–22 ms (passes com timestamp: malhas do passe principal 44%, sombras virtuais 14%, TLAS
9%): 20 mil formas básicas Movable sem LOD.

**Meta de 200 fps com 20 mil não alcançada** (11–14 fps; 2026-11-19: ver "20 mil corpos" no fim). O que faltava, medido:
1. Solver SIMD (AVX2, 8 contatos por vez, como o "wide" do Box2D v3): o solver escalar é ~2000 ciclos por contato por
   passo; estimativa 6–8 → ~2 ms.
2. Pares e cores persistentes (hoje refeitos todo passo, a coloração serial 1,1–1,5 ms) e grade montada em paralelo
   (1,3 ms serial) ou na GPU.
3. Render: 20 mil formas Movable custam 15–22 ms de GPU sozinhas (LOD/instancing das formas básicas: área do render).
4. A 200 fps o passo de 120 Hz cai em 60% dos quadros: o custo médio por quadro é ~0,6 passo.
Com quase tudo dormindo (o caso comum num jogo) a física não custa: só os acordados entram no passo.

**Testes** (área `fisica`): `phys_stack`, `phys_stack_awake`, `phys_ccd`, `phys_joints`, `phys_heavy` (mede: passa
enquanto ninguém atravessa o chão nem sai da arena); os antigos `physics`, `physics_stress`, `physics_stress_shapes`,
`physics_shapes`, `constraints`, `char_physics` seguem.

**API para o Q9/Q10** (`physics.h`): as funções antigas iguais; novas: forças, torques, velocidades, massa, sono,
gravidade, material, CCD e as das juntas (drive, limites, movimento por eixo, força). `CRigidBody` e `CConstraint` só
ganharam campos no fim.

## 20 mil corpos (2026-11-19, sobra do Q8)
Pedido: `phys_heavy` (ou uma cena equivalente de 20 mil corpos) a ≥ 200 fps no Épico, sem perder a estabilidade.

**O que mudou** (medido em pares alternados com a base `81345b4`, i7-11700F, RTX 3060, 2560×1417, gráficos Épico):
- **Solver AVX2** (`phys_simd.h`, "wide" do Box2D v3): os contatos de uma cor em lotes de 8 (`Wide`, SoA, até 4 pontos);
  os corpos lidos como 8 linhas de 32 bytes transpostas (`State`: v im w dyn | I; `Delta`: dp dq) e escritos de volta
  só nas faixas acordadas. Dentro da cor os contatos vão pelo número de pontos (4 primeiro: o laço do lote para no
  maior). A cor de transbordo continua escalar. Fallback de 8 floats em C puro (Android: o NDK vetoriza com NEON).
  Com `REALENGINE_PHYS_SCALAR=1` o resultado é **idêntico** ao da base (mesmos passos, mesmos EPA); com o AVX2 só o
  arredondamento (FMA) muda — num monte caótico o assentar varia (`physics_stress_shapes` 8,6 → 11,1 s, `physics_stress`
  7,2 → 6,9 s), as pirâmides não.
- **Integração** sem ler `Body`/`CRigidBody` (`IParam` por corpo: gravidade, amortecimentos, giro máximo): integrar
  1,55 → 0,39 ms por passo.
- **Grade em paralelo** (`grid_build_par`: contar, escrever, contar por balde com atômicos, somar em blocos, espalhar,
  ordenar cada balde por corpo e célula: a ordem da serial, determinístico) com os limites dentro da entrada (os pares
  não leem `Body`). A grade dos dormindo tem tamanho de célula próprio: antes seguia a dos acordados, que muda todo
  passo, e era refeita todo passo (0,67 ms por passo no armazém).
- **Coloração** lendo só os pontos de cada manifold (`mnp`), `ctz` por instrução.
- **Sincronizar e escrever de volta em paralelo** (pedidos de acordar/dormir e o log de mudanças depois, em ordem).
- **Pool de threads** (notas-dev/jobs.md): não espera mais os workers que dormem; cada chamada pagava ~0,1–0,3 ms.
- **Render** (escala.md, gi.md): comparação do conjunto dinâmico em paralelo (CPU 3,0 → 1,9 ms com 20 mil), TLAS dinâmico
  por refit, cache de superfície dos que se movem numa lista (1,69 → 1,40 ms).
- Testado e descartado: **reaproveitar manifolds** entre passos quando o par quase não se moveu (o contact cache do
  PhysX): com 0,1 cm a pirâmide andava 3,9 cm; com 0,02 cm a acordada andava 0,31 cm e o ganho era ~2% do narrowphase.
  **Workers girando** durante o passo: o Windows tirava a thread principal do núcleo (25 → 59 ms por passo).
  **Menos threads no solver**: 4, 8, 12 ou 16 dão o mesmo (~5,5 µs por barreira × 192 barreiras: latência de cache
  entre núcleos, não falta de núcleo).

| Cena (gráficos Épico) | Base | Agora |
|---|---|---|
| `phys_stack` (1035 caixas, dormem) | 5,10 ms/passo | **1,59** (parada, 3,21 cm afundada: igual) |
| `phys_stack_awake` (6 sub-passos) | 6,45 ms/passo, 0,24 cm | **1,72**, 0,34 cm (limite 0,5) |
| `phys_heavy` (20 mil acordados, 48 mil contatos) | 22,0 ms/passo (broad 4,3, narrow 6,1, solver 11,0), 9–10 fps; GPU mín. 9,2 ms | **17,2** (3,2 / 5,9 / 7,5), 12 fps; Médio 16 fps; GPU mín. 8,3 (GI 2,45 → 1,36) |
| `phys_crates` (20 mil dormindo, explosão a cada 1,5 s, até ~3000 acordados) | — (cena nova) | Épico **143 fps** (física 3,4 ms/quadro), Médio **163 fps** (física 1,8; CPU 4,9 ms, GPU 5,9 ms, mín. 4,6) |
Estágios do solver na pesada (ms por passo): pack 1,06, integrar 0,39, aquecer 1,29, resolver 1,82, relaxar 1,76,
restituição 0,24, guardar 1,22.

**Por que 200 fps com 20 mil acordados não dá nesta CPU (8 núcleos)**: a 200 fps o quadro tem 5 ms, e a 120 Hz o passo
cai em 60% dos quadros: o passo teria que caber em ~3 ms com o render junto. Só as 192 barreiras do solver (15 cores ×
3 passadas × 4 sub-passos) custam ~1 ms, e cada lote de 8 contatos é limitado pela troca de linhas de cache entre
núcleos (~1 µs). O que falta para chegar perto, em ordem: pares e cores persistentes (Box2D v3: só os que entram e saem;
hoje coloração serial 1,1 ms e 97 mil pares refeitos), narrowphase incremental (5,9 ms: 9 mil GJK por passo nos
cilindros e cápsulas), menos cores (as 15 cores de um monte 3D), e física na GPU para escalas acima disso.
O caso de jogo (milhares acordados de uma vez, o resto dormindo) é a cena `phys_crates`; ali o limite passa a ser a
GPU: passe principal 2,0 ms (os Movable em movimento sombreados pela GI de contato) e cache de superfície 1,4 ms
(96K texels por quadro dos que se movem) — próximos passos da área do render.

**Preset de escala: Physics Quality** (Project Settings > Physics; `p.Rate` no console e `--cvar p.Rate=60` no
editor também): **Épico** 120 passos/s × 4 sub-passos (o padrão), **Alto** 90 × 4, **Médio** 60 × 4 (o padrão do
próprio Box2D; contatos de 30 Hz: uma pilha de 45 andares afunda ~14 cm em vez de 3), **Baixo** 60 × 2. É do projeto,
não do jogador: a física é a mesma em toda máquina (multiplayer, replays). Campo novo `physics_rate` no `.rproject`
(projeto antigo abre com 120).

**Testes**: `phys_crates` (Médio, `--min-fps 100`, nada atravessa o chão) entrou na área `fisica`; `phys_heavy`
segue sendo a medida; o relatório das duas mostra as fases em todos os passos, os estágios do solver, as cores, os
pares e o custo por quadro fora dos passos (sincronizar, escrever de volta).
