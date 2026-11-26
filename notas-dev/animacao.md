# Animação (`src/anim.c`, `src/anim_editor.c`)

Skeletal Mesh, Animation Sequence, Anim Montage e Animation Blueprint, como na Unreal.

## Assets
- **`.rskm` Skeletal Mesh**: esqueleto (ossos com pai e pose de ligação) + **partes** (uma malha presa a um osso:
  "skinning segmentado"). Cada parte é uma instância comum do renderer (culling e LOD na GPU, sombras e GI por raio):
  por isso escala para milhares de personagens. Skinning suave por pesos (malhas importadas do Tlender) é o passo
  seguinte do renderer (registro de malhas).
- **`.ranim` Animation Sequence**: uma trilha por osso (rotação; translação), 30 fps, interpolação nlerp; **notifies**
  (nome + tempo) chamam o evento `AnimNotify_<Nome>` do Anim Blueprint e do ator.
- **`.rmontage` Anim Montage**: sequência + slot (`DefaultSlot` = corpo todo; `UpperBody` = do osso superior do Anim
  Graph para cima, as pernas seguem a locomoção), blend in/out, velocidade, seções.
- **Animation Blueprint**: classe Blueprint com pai **Anim Instance**. Event Graph pela VM (nós: Event Blueprint Update
  Animation, Try Get Pawn Owner, Get Velocity, Is Falling...); **AnimGraph** (aba no editor de BP): máquina de estados
  com estados *Sequence Player*, *Blend Space 1D* (até 4 amostras por um float; fases sincronizadas) e *Blend Poses by
  Bool*; transições por variável (bool verdadeiro/falso, float >/<, fim da animação) com tempo de mistura; depois o slot
  dos montages.
- Nós de Blueprint: **Play Montage**, **Stop Montage**, **Play Animation** (o asset escolhido nos Detalhes do nó).

## O que a engine cria (Content/Characters/Mannequin, só no editor e só se faltar)
`SK_Mannequin` (21 ossos, 28 partes), `MM_Idle`, `MM_Walk`, `MM_Run` (com notifies `Footstep`), `MM_Jump`, `MM_Wave`
(montage `AM_Wave` no UpperBody) e `ABP_Mannequin` (Speed/IsInAir; Locomotion Idle→Walk→Run por Speed, Jump por
IsInAir). Um **Character novo** já vem com o componente `Mesh` (Skeletal Mesh) com esse ABP, os pés na base da cápsula.

## Runtime
- `CSkeletalMesh` num ator (ou num componente de Blueprint): as partes viram entidades filhas (`CAttach` tipo
  `ATTACH_SK_PART`) e, com Anim Blueprint, uma entidade da instância (`ATTACH_ANIM_BP`, a VM roda o Event Graph dela).
- Por quadro: lógica (máquina de estados, notifies, montage) na thread principal; poses em paralelo
  (`re_parallel_for`, blocos de 64); o que a câmera não vê (relevância da GPU) atualiza a cada 4 quadros. Partes
  Movable não são "tocadas" (o renderer compara o conjunto dinâmico sozinho).
- Só as 4 maiores partes de cada personagem são oclusores do AO de contato (cada oclusor custa todo pixel).
- Editor: as partes ficam na pose de ligação onde o ator estiver, refeitas quando o mundo muda **ou** quando a
  transformação de um Skeletal Mesh muda (um hash delas a cada quadro, alguns ns cada). O gizmo arrasta a seleção no
  lugar sem registrar mudança: o gizmo andava e a Delight ficava parada (relatório #114). Teste `skeletal_gizmo_move`
  (`--sk-move-test`): o Mannequin movido 300 cm, as 28 partes seguem, e um quadro parado não refaz nada.

## Editores
- **Skeletal Mesh Editor**: árvore de ossos, viewport (prévia na GPU com materiais e modos de visualização, `editor.md`; ossos em linhas), detalhes do osso (pose de
  ligação) e das partes (malha, posição, rotação, tamanho, cor), animação de prévia, navegador de animações.
- **Animation Editor**: timeline com play/pausa, velocidade, loop, arrastar para navegar, **notifies** (adicionar no
  cursor, arrastar, clique direito apaga), Asset Browser com as outras animações e montages do esqueleto, Criar Montage.
- **Montage Editor**: animação, slot, blends, velocidade, seções.
- Conteúdo: Adicionar > Animação > Animation Blueprint / Anim Montage; duplo clique abre o editor.
- Detalhes do nível e do componente: Skeletal Mesh, Anim Class (ou Anim to Play + velocidade + loop).

## Medidas (RTX 3060, 1440p)
| Cena | Resultado |
|---|---|
| `--scene anim1000 --play` (1000 Mannequins, 28 000 partes) — antes das otimizações | 16 fps (CPU 61 ms: o log de mudanças estourava e reconstruía a cena) |
| sem tocar partes Movable | 45 fps |
| instâncias dinâmicas em paralelo + cópia única | 48 fps |
| TLAS dinâmica montada na pilha (não ler memória write-combined) | 68 fps (14 → 5,6 ms) |
| relevância copiada da GPU para a RAM (leitura não cacheada) | 72 fps (5,6 → 0,3 ms) |
| cache de superfície: despachos agrupados + orçamento de 96K texels | **99 fps** (CPU 5,0 ms, GPU 10 ms: shading da multidão) |
| animação em si (1000 personagens) | **0,55 ms** CPU |
| Character com Mannequin (house_walk / play_flicker) | 376–444 fps (oclusores limitados a 4 partes) |
Teste `anim_crowd` (≥ 80 fps, 1000 personagens).

## Blend Space 2D, Aim Offset, Foot IK (2026-10-31)
- **Blend Space 2D** (estado do Anim Graph "Blend Space 2D"): até 9 amostras em (X, Y) por duas variáveis float.
  Pesos por **gradient band** (o "2D Freeform": funciona com qualquer disposição de pontos, não só grade), com os
  eixos normalizados pelo intervalo das amostras (velocidade em cm/s e direção em graus pesam igual); fora de todas, a
  mais próxima. As amostras tocam na mesma fase (ciclo = soma ponderada dos comprimentos: os pés concordam) e se
  misturam por `anim_blend_n` (rotações alinhadas ao primeiro hemisfério e normalizadas). No editor: X/Y por amostra,
  remover, e um quadro com os pontos — passar o mouse mostra os pesos ali.
- **Aim Offset** (do grafo inteiro, depois dos montages): Yaw e Pitch (graus, variáveis float; + yaw para a direita do
  personagem, + pitch para cima) repartidos em até 4 ossos (coluna até a cabeça, com a parte de cada um), girando em
  torno dos eixos cima/direita do ator. Hoje é procedural (sem poses de mira em assets).
- **Foot IK** ("Pés no chão", só jogando): um raio vertical sob cada pé nas caixas do mundo (`bp_trace_boxes`, as mesmas
  do Character — agora montadas também quando há Skeletal Mesh), o deslocamento de cada pé suavizado, a pélvis desce
  pelo pé mais baixo, cada perna dobra por **Two Bone IK** (`anim_two_bone_ik`: o joelho dobra para o lado em que estava
  em relação à reta quadril-pé) e o pé mantém a rotação. Personagem no ar: desliga suave. Os ossos são achados uma vez
  por revisão do grafo (`AnimGraph.rev`).
- O ABP do Mannequin novo já vem com Foot IK e a cadeia de mira (spine_01, spine_02, spine_03, head).
- **Testes**: `anim_math` (`--anim-selftest`: pesos 2D — 1 em cima de uma amostra, soma 1, meio da célula dividido
  pelos 4 cantos, fora = a mais próxima; IK alcança o alvo mantendo os comprimentos e o joelho para frente; fora de
  alcance reto; misturar pose igual não muda) e `anim_foot_ik` (`--scene footik`: pé esquerdo sobe 25 cm no degrau,
  o direito fica, a pélvis fica). Conferido na tela, inclusive o Aim Offset (yaw 40°, pitch 25°).
- **Medido**: um Mannequin com Foot IK e Aim Offset: 0,013 ms de animação por quadro (a pose, 6 recomputações do
  mundo de 21 ossos e 2 raios); em multidão roda nos jobs paralelos como as poses.

## Motion Matching (2026-10-31, `src/anim_mm.inc`)
- Estado novo no Anim Graph: **Motion Matching** (Plays > Motion Matching): a lista de animações vira um banco de
  quadros (30/s) com 14 traços: velocidade do corpo agora e em 0,3 s, posição e velocidade dos dois pés (no espaço da
  raiz). A cada 0,1 s busca o quadro mais próximo do que o pawn faz (variável Speed, Direction opcional, aceleração
  suavizada) e da pose atual; troca se for 15% melhor, com transição de 0,2 s (a antiga continua tocando por baixo).
- **Animações geradas no estilo das existentes**: cada clipe em movimento entra com time-warp (0,8x a 1,25x: outras
  velocidades) e espelhado (_l/_r, Left/Right trocados: o outro pé primeiro), se o esqueleto for simétrico (conferido
  nos binds). Mannequin: 3 clipes → 22 grupos, 674 quadros, 0,7 ms para montar.
- **Velocidade de cada clipe** medida pelo pé de apoio (pé a até 2,5 cm da menor altura do clipe, nos dois quadros):
  ele recua sob o corpo tão rápido quanto o corpo anda. Mannequin: Walk 217 cm/s, Run 600 cm/s (o ABP_Mannequin usava
  160/500 no Blend Space — daí o deslize). Root motion, se houver, tem preferência. A taxa de reprodução ainda se
  ajusta à velocidade real (0,85-1,18x).
- `ABP_Mannequin_MM` (novo, criado nos projetos): o mesmo grafo do ABP_Mannequin com Locomotion por Motion Matching.
- Onde achar: botão direito no Anim Graph > **Add Motion Matching State** (já com a variável Speed e as animações em
  loop do esqueleto como banco); o card do estado mostra "Motion Matching: Speed". Para usar num Character: Anim Class
  = ABP_Mannequin_MM.
- Nas IAs: qualquer AI Character com esse ABP (teste `ai_squad_mm`); na **multidão**, o mesmo princípio sem busca:
  a fase de cada agente anda pela velocidade de chão medida dos clipes (`anim_seq_ground_speed`), os pés não deslizam.
- Medida nova nas estatísticas: **deslize dos pés** = quanto o pé de apoio (o mais baixo, no chão, o mesmo do quadro
  anterior) anda no mundo dividido pelo percurso do corpo (só andando > 50 cm/s).

| `--scene ai_squad` vs `ai_squad_mm` (4 caçadores, 6 s) | Blend Space 1D | Motion Matching |
|---|---|---|
| Deslize dos pés | 318% do percurso | **25%** |
| Custo | — | 276 buscas, 1,17 ms no total (0,004 ms por busca) |
| FPS | 420 | 418 |

## Personagens próprios, pacotes e inclinação (2026-10-31, `src/anim_pack.inc`, `src/anim_creatures.inc`)
- **SK_Mannequin_Male / SK_Mannequin_Female** (criados nos projetos ao lado do Mannequin): o mesmo esqueleto (tocam
  todas as MM_*), proporções próprias — ombros, peito arredondado, cintura, quadril, membros, pescoço, cabeça; a
  feminina com rabo de cavalo. Feitos por código (formas por osso): da engine, não de terceiros.
- **Pacote de Locomoção** (Ferramentas > Pacotes de Animação): andar de costas, strafe esquerda/direita, sprint,
  agachado parado/andando, pulo em 3 partes (início, queda, aterrissagem), rolamento, escada, vault — em
  `Characters/Mannequin/Locomotion`. Os que se movem entram no banco do Motion Matching do ABP_Mannequin_MM; sem
  variável Direction, o MM usa a direção do próprio pawn (velocidade contra a frente: o Calculate Direction da Unreal),
  então o strafe e o andar de costas são escolhidos sozinhos.
- **Criaturas** (mesmo menu): Quadrúpede (cachorro: passo em pares diagonais com fase de apoio 60%, galope com os pares
  dianteiro/traseiro e a coluna flexionando, cauda, ofegar), Peixe (onda que percorre o corpo, amplitude crescendo até a
  cauda, nadadeiras), Ave (asas em 3 segmentos: descida forte, subida dobrada; planar; parada olhando em trancos;
  pulinhos), Réptil (lagarto: pernas abertas para o lado, onda em S da coluna e da cauda, pares diagonais). Cada um:
  esqueleto, partes, clipes e um ABP (Locomotion por Speed) em `Content/Creatures/<nome>`.
- **Inclinação (Lean)** no Anim Graph (padrão 0,5 nos ABPs novos): o tronco rola para dentro da curva (taxa de giro) e
  inclina ao acelerar, repartido nos ossos da coluna do Aim Offset.
- Teste `anim_packs` (`--scene packs`): 29 arquivos feitos, 8 personagens/criaturas tocando, 0,024 ms de animação.

## Auto Rig (2026-10-31, `src/autorig.inc`)
- Detalhes de um ator de Static Mesh importada > **Auto Rig (Humanoid)**: vira Skeletal Mesh no esqueleto do Mannequin
  (todas as animações e pacotes tocam nele). Escala pela altura (o ator recebe a escala inversa), pés no chão, meio no
  eixo, frente em +X; **T-pose detectada** pela largura (> 0,65 da altura) e os braços baixados (girados no ombro);
  cada triângulo vai para o osso cujo segmento está mais perto do seu centro (partes rígidas por osso). A estátua fica
  escondida; o novo ator ocupa o lugar com MM_Idle.
- Teste `autorig` (`--scene autorig`): estátua em T-pose (as partes do manequim masculino numa malha só) → 20 ossos
  com triângulos, escala 1,01, andando; 215+ fps.
- Limite: juntas esféricas divididas entre dois ossos podem mostrar quinas; só humanoides de pé.

## Skinning suave (2026-11-07, vídeo do usuário: "o vestido dela tá todo cortado" e um flick no peito)
- **O defeito:** as malhas deformadas eram cortadas em partes rígidas, uma por osso (a mais forte de cada face). O
  vestido saía em tiras (os pedaços de cada osso do vestido não dobravam juntos) e, com a pose mudando, as camadas
  de roupa (avental no Spine_03, blusa no Spine_04) se desencontravam: emenda horizontal no peito e listras da blusa
  atravessando o avental, que mudavam com a distância (o flick no peito).
- **Agora:** cada material vira uma **pele** (`.rskm` v2, `skins`; malha `SKS_<nome>_<material>.rmesh` com
  `RMESH_SKINNED`): os vértices na pose de ligação, cada um com os **4 ossos mais fortes** e pesos (unorm8 somando
  255) guardados nos bits do UV2 (uma malha sem atlas não usa UV2; assim os pesos seguem a solda e a ordem do
  meshoptimizer). Tlender (grupos de vértices), glTF (`JOINTS_0`/`WEIGHTS_0`) e FBX (clusters, os 4 maiores por ponto).
  Malhas presas a um osso sem pesos entram na pele com peso 1 nele. Arquivos v1 (só partes) seguem abrindo.
- **Runtime (como o Skin Cache da Unreal):** a pele é uma entidade (`ATTACH_SK_SKIN`) no ator, no conjunto dinâmico.
  `anim.c` calcula a **paleta** por ator (pose no espaço da malha × inverso da ligação, `anim_skin_palette`, versão a
  cada mudança). O renderer (`render.c` "skin") dá a cada pele duas áreas próprias no buffer de vértices (depois das
  malhas, `skin_room`: 8× os vértices das peles, até 6 M; faltou, as malhas são recarregadas com mais), e quando a
  versão muda o `skin.comp` dobra os vértices (último quadro copiado para a segunda área: o passe de velocidade) e a
  **BLAS própria** é atualizada por refit (build a cada 64). A instância aponta para a área (`INST_SKINNED`,
  `ids[3]`); `mesh.vert`, `velocity.vert` e os hits do `rt.glsl` leem por ela. Pose parada: nada roda.
- Raio do culling da pele: o da ligação × 1,35 (braços e chutes passam dele). Sem clusters nem LODs (uma pele).
- Personagens importados antes continuam em partes: **reimportar** o arquivo dá as peles.
- Teste `skin_gpu` (rig.fbx importado e jogado, validação, `Skins: 1, bent last frame 1`); `import_glb` agora 7
  malhas (8 antes), `import_fbx_rig` 1 (5 antes).
- Medidas (RTX 3060, 1440p, Delight importada do `.tlend`, 5 peles, 57 099 vértices, animação tocando):
  495 fps, GPU mínima 1,49 ms (main 0,89), CPU das paletas 0,004 ms, validação limpa.

## Morph targets (2026-11-08, item B4b do plano; blend shapes / shape keys em tempo de execução)
- **Formato:** `.rmesh` v3 (`RMESH_VERSION 3`, `render.h`): com `RMESH_MORPHS` nos flags, depois dos índices vem o
  bloco: `count`, `nd`; os nomes (48 bytes) e os pesos padrão; por vértice o primeiro delta (CSR, `nverts + 1`); os
  deltas `RMeshMorphDelta` (alvo, Δposição, Δnormal: 28 bytes, esparsos: só os vértices que o alvo mexe). Sem o flag
  é o layout da v2 (a paisagem e as malhas sem morph não mudam); v1 e v2 continuam abrindo (o cache das v1 manteve o
  nome `_v2`). Máximo 64 alvos por malha (`RMESH_MAX_MORPHS`).
- **Solda e ordem (`mesh_build.c`):** o importador entrega os deltas por **vértice de origem** (`MeshMorphs`, CSR) e,
  por canto, o vértice de origem dele (`csrc`). `mesh_build_skinned` solda com `meshopt_generateVertexRemapMulti`
  (o `GpuVertex` + a origem: dois vértices iguais de origens diferentes não se fundem), a ordem do meshoptimizer usa
  `optimizeVertexFetchRemap` e leva a origem junto (`order(..., tag)`), e no fim cada vértice copia os deltas da sua
  origem (`morphs_gather`).
- **Skeletal Mesh:** `.rskm` ganhou `"morphs"` (nome + peso padrão): a lista do ator; as peles casam os alvos delas
  com ela **pelo nome** (dois objetos com "Smile" = um controle só). Malha com morph **sem armature** vira um Skeletal
  Mesh de **um osso** (`Root`, identidade), uma pele por slot, e o ator um `CSkeletalMesh` na transformação do objeto;
  o skin job de um osso dobra só pelos morphs.
- **Importadores:** Tlender (`.tlend` v33 `shapekeys`: modo Relativo; cada chave menos a Basis é um alvo, o
  deslocamento menos o da chave relativa, vezes o grupo de vértices; mutada = peso 0; modo Absoluto ignorado). glTF:
  `targets` POSITION/NORMAL, nomes de `mesh.extras.targetNames` (senão `Target_N`), pesos de `mesh.weights` (antes
  eram aplicados na malha na importação; agora ficam como padrão). Sem NORMAL no alvo (Tlender e glTF estático), a
  Δnormal é a normal suave da malha com o alvo inteiro menos a da malha (o vizinho de um vértice que andou gira junto).
  FBX: blend shapes ainda não (próximo passo).
- **Pesos por ator:** `CSkeletalMesh.morph_weight[64]` + `morph_set[2]` (um bit por alvo) no **fim** da struct: um nível
  antigo lê zero = nenhum definido = os padrões do arquivo. `anim_morph_weight` (o do ator se definido, senão o
  padrão), `anim_morph_set/get` (nome sem diferenciar maiúsculas; um skin ou parte pede pelo dono).
- **GPU (`skin.comp`, `render.c` "skin"):** um buffer de morphs (device local, junto do upload das malhas): por malha
  o CSR e os deltas. O `SkinJobGpu` (64 bytes) ganhou `nmorph`, `mfirst`, `mdelta`, `mweight` (os pesos vão depois da
  paleta, no mesmo buffer por quadro). No shader, antes dos ossos: `pos += w × dp`, `n += w × dn`, normalizada.
  `nmorph = 0` quando todos os pesos são zero: o laço nem roda. A pele só é dobrada quando a pose **ou o hash dos
  pesos** muda: parada, custo zero (o teste confere).
- **Editor e Blueprints:** Details > Skeletal Mesh > **Morph Targets** (um controle 0–1 por alvo, Ctrl+clique digita
  qualquer valor; **Restaurar Morph Targets** limpa os bits). Nós **Set Morph Target** e **Get Morph Target**
  (Components|Skeletal Mesh; Target, Morph Target Name, Value), `BPK_MORPH_SET/GET`. Artigo `morph-targets`.
- **Teste `morph_targets`** (`morph_test.c`): escreve um glTF (grade 128×128 em pé, 16 384 vértices, "Wave" 0,5 por
  padrão e "Bulge" no meio), importa, lê os vértices da pele de volta da GPU (`render_skin_readback`), põe Wave 1 e
  Bulge 1 por `anim_morph_set` e confere cada vértice contra a fórmula (erro < 0,05 cm); mede com os pesos mudando a
  cada quadro, sem os morphs (`render_morph_off`: o custo dos ossos) e com; depois parado (0 dobras). Validação
  limpa. `import_gltf` agora "1 skeletal meshes" (a fita com o morph virou Skeletal Mesh de um osso).
- **Medidas** (RTX 3060, 2560×1417, teste `morph_targets`; grade de 16 384 vértices, 2 alvos, ~1,9 deltas por vértice,
  dobrada a cada quadro): GPU do `skin.comp` (timestamps 16–17, `RenderStats.gpu_skin_ms`) **0,0109 ms só os ossos,
  0,0136 ms com os morphs (+0,0026 ms ≈ 0,16 ns por vértice)**; CPU do `skin_bind` 0,0023 → 0,0033 ms (pesos lidos
  e hash); pesos parados: **0 dobras** em 30 quadros (custo zero). A cena do teste mede 531 fps. Erro dos vértices
  lidos de volta contra a fórmula: 0,0000 cm. `.tlend` com shape keys: teste `import_tlend_shapekeys`
  (`tests/data/import/shapekeys.tlend`: Up 50 cm × o grupo de vértices, Wide ±30 cm relativa a Up). A linha `Skins:`
  do `--bench` agora mostra a GPU, e há uma linha `Morph targets:`.
- **Próximos passos:** (curvas de morph: feitas em 2026-11-09, "Curvas de morph" abaixo), curvas de shape key do
  Tlender, blend shapes do FBX, morphs nas multidões.

## Limites / próximos passos
- Aim Offset por poses (assets), Root Motion, IK de mãos por alvo. Peles nas multidões (hoje partes rígidas).
- Retarget entre esqueletos (hoje as trilhas casam pelo nome do osso).

## AnimGraph como o da Unreal (2026-11-01, `src/anim_nodes.inc`, `src/anim_graph_ui.inc`)
- **Grafo de poses** (aba AnimGraph, que abre na frente ao abrir um Animation Blueprint): nós com pinos de pose até o
  **Output Pose**. Nós: State Machine (até 4 máquinas), Sequence Player, Blend Space 1D Player, Blend Space Player (2D),
  Motion Matching, Blend Poses by bool / by int (com tempo de mistura), Blend (alfa), Layered blend per bone (filtro de
  ramo), Apply Additive, Slot (DefaultSlot / UpperBody), Two Bone IK, Leg IK, Aim Offset, Lean, Transform (Modify)
  Bone, Look At (vira para a câmera), Save / Use Cached Pose (avaliada uma vez por quadro), Inertialization (um salto na
  entrada some suave), Mirror.
- Canvas: botão direito = menu por categoria com busca ("Todas as Ações deste Grafo"); arrastar de um pino de saída
  para uma entrada liga, puxar de uma entrada solta, Alt+clique desfaz, soltar no vazio abre o menu e liga o nó novo;
  Del/X apaga, Ctrl+D duplica, Home enquadra; duplo clique numa State Machine entra nela (trilha "AnimGraph > ...").
- **My Blueprint** (sugestão #72): a seção Grafos lista o **AnimGraph** acima do EventGraph, como na Unreal; o clique
  traz a aba (reabre se estava fechada) e a linha fica marcada enquanto ela é a aba atual.
- **Máquinas de estado**: estado "Qualquer Estado" (State Alias da Unreal), transição com até 3 regras (E), regras novas
  (int ==, tempo no estado >), **prioridade** e **curva** (linear, cúbica, ease out).
- Grafos antigos viram nós ao carregar (máquina → Slot DefaultSlot → Slot UpperBody → Lean → Aim Offset → Leg IK →
  Output): mesma pose, testes `anim_*` passando. Autoteste novo no `anim_math` (cache, Modify Bone, Blend, bool, Mirror).

## Editor de Motion Matching (2026-11-01, `src/anim_mm_editor.inc`)
- Abre pelo botão **Abrir Editor de Motion Matching** num estado ou nó de Motion Matching (ou `--mm-editor <ABP>`).
- **Banco de Dados**: as animações (velocidade medida), os grupos que cada uma gera (0,8x-1,25x e espelhados) — clicar
  num grupo toca ele no viewport com uma linha do tempo. **Depurador de Busca**: a busca do jogo rodando ao vivo a
  partir de velocidade, direção e aceleração escolhidas (a cada 0,1 s, troca se 15% melhor, mistura 0,2 s), a
  trajetória vista de cima (pedida × do quadro, pés), as velocidades de todos os grupos e o custo ao longo do tempo.
- O banco agora é achado pelo conteúdo (clipes + malha + revisão do grafo), não pelo ponteiro do estado: o nó Motion
  Matching e o editor usam o mesmo do jogo.

## Animações da engine refeitas (2026-11-01, `src/anim_motion.inc`)
- Cada osso em quaternion (`QPose`); as pernas por **IK de dois ossos** até pés que plantam: no apoio o pé fica parado
  no mundo e rola calcanhar → chapado → ponta (o tornozelo sobe girando nos dedos); no balanço sobe (a corrida chuta o
  calcanhar para trás) e avança num caminho suavizado. A pélvis sai do alcance das pernas (pêndulo invertido: o andar é
  mais alto sobre o pé de apoio, a corrida mais baixa no meio do apoio), desloca para o pé de apoio, cai do lado que
  balança e gira com as pernas; o tronco contrarrota; a cabeça fica nivelada; braços com cotovelo e mão atrasados
  (follow-through) e clavículas que sobem com o balanço.
- Idle em contrapposto (peso numa perna, o outro joelho solto), respiração no peito e ombros, peso e olhar à deriva;
  ações por poses-chave com antecipação, exagero e acomodação (impulso do pulo, aterrissagem, aceno, rolamento).
- Velocidades medidas pelo Motion Matching: andar 116, **trote (MM_Jog, novo) ~250**, corrida 411, sprint 642 cm/s.
  Blend Space do ABP_Mannequin: 0 / 120 / 260 / 420. `ai_squad` (Blend Space): deslize dos pés 318% → **35%**;
  `ai_squad_mm` 43%.
- `.ranim` com `generator`: clipes da engine de uma geração antiga são refeitos ao abrir o projeto (notifies mantidos);
  pacotes de criaturas também (cachorro com pata curvando no balanço, ave com batida de asa realista).
- `--anim-sheet <animação> <png> [--sheet-yaw graus]`: 8 quadros lado a lado (revisão das animações).

## Motion Matching: animações geradas (2026-11-01, `src/anim_mm_gen.inc`)
Pedido: não precisar de centenas de animações. Com **um idle, um andar e uma corrida** (e, se quiser, uma para o lado) o
banco ganha sozinho, no estilo dos clipes, assadas a 30 quadros/s junto com o banco (nos workers):
- **Direções intermediárias** (a cada 22,5°) entre dois clipes de velocidade parecida (até 2,5x: strafe costuma ser mais
  lento) sem outro entre eles: os dois na mesma fase (a partir do pouso do pé esquerdo); o peso acha o ângulo; o que o blend
  perde de velocidade volta metade na cadência e metade na passada (raiz quadrada). Também a 0,85x e 1,18x.
- **Starts** (idle → cada locomoção, inclusive as direções geradas; com o idle e o espelhado) e **stops** (cada uma →
  idle, de **8 pontos do ciclo**): a velocidade sobe numa rampa de aceleração constante com as pontas suavizadas (sem
  tranco); peso do blend e cadência crescem com a **raiz da velocidade** (a "walk ratio": passo/cadência constante).
  O start começa na fase em que os pés da locomoção estão mais perto dos do idle (o pé livre pisa primeiro). O stop vai
  para o idle (ou o espelhado) que exige menos correção; um pé que ficou para trás dá um **passo de fechamento** (sobe em
  arco, um pé de cada vez, com o outro apoiado e o corpo parado).
- **Trocas de velocidade** entre vizinhas da mesma direção e postura (andar↔trote↔corrida↔sprint), nas duas direções.
- **Física**: inclinação do corpo pela aceleração (atan(a/g) numa mola amortecida: balança ao parar; pélvis 40%, coluna
  60%, cabeça compensa metade), pélvis desce junto; **pés apoiados presos no chão** (o apoio é a altura de pé plantado
  misturada das fontes: a corrida pisa mais alta que o idle), o pé no ar leva a correção de um apoio ao próximo (stride
  warping), pélvis baixa onde a perna não alcança, pernas por Two Bone IK.
- **Estilo/peso tirado dos clipes**: passos até a velocidade = 1,5 + 0,75·Fr (Froude v²/gL, máx. 2) ao arrancar e
  1 + 0,6·Fr ao parar, cada passo meio ciclo do próprio clipe (cadência lenta = arranque lento); limites humanos
  0,08–0,75 g / 0,1–0,8 g. Mannequin: andar 134/197, trote 308/439, corrida 391/533, sprint 736/785 cm/s².
- **Controle do usuário** (Editor de Motion Matching > Ajustes das Animações Geradas; `AnimState/AnimNode.mmgen`, salvo no
  .rbp): liga/desliga starts e stops, trocas de velocidade, direções e "Movimento Segue o Corpo"; aceleração e
  desaceleração (Automático = pelos clipes), peso, inclinação, acomodação, passo do ângulo. A lista "Animações Geradas"
  toca cada uma no viewport, com "Andar Sobre o Chão" (o deslize aparece contra a grade).
- **Em jogo**: o Character acelera/freia no limite do corpo (`CCharacter.anim_accel/anim_decel`, só enquanto o MM toca;
  o freio fica o da velocidade em que a parada começou); a busca usa a velocidade pedida (`CCharacter.desired`: a
  trajetória da Unreal) — o start/stop entra no mesmo quadro da entrada; start/stop em andamento só troca se outro for 2x
  melhor; o tempo deles segue a **distância** andada pelo corpo (Distance Matching), então o pé apoiado fica parado
  mesmo se a curva de velocidade não for igual. No fim, segue no loop (mistura de 0,12 s).
- Busca com poda por caixa de features por grupo (pula grupos mais longe que o melhor até agora).
- Métrica de deslize corrigida (anim.c): "pé no chão" = até 3 cm da altura plantada recente do próprio pé (antes: tornozelo
  a menos de 8 cm do chão, mas o do Mannequin parado fica a 8,5 cm — contava ~2% do percurso).

| Medida (RTX 3060) | Só os clipes | Com as geradas |
|---|---|---|
| Deslize das geradas, assadas | — | direções 0%, starts 1,4%, stops 4,2%, trocas 1,6% (os clipes da engine: 10%) |
| `--scene mm_startstop` em jogo (idle/andar/parar/correr/parar/trote/parar) | 23,8% | **17,9%**; 11,3% durante starts/stops |
| `--scene ai_squad_mm` | 48,7%, 153 trocas | **29,3%**, 63 trocas |
| Banco do ABP_Mannequin_MM (7 clipes) | 72 grupos, 2 088 quadros, 2,5 ms | 310 grupos (12 direções, 38 starts, 152 stops, 12 trocas), 10 488 quadros, 12,5 ms (geração 10,7 ms, uma vez) |
| Busca | 0,002 ms | 0,007 ms (com poda) |
| FPS | — | mm_startstop 580–600, ai_squad_mm 414 |
Teste `mm_startstop` (≥ 2 starts e 2 stops tocados, geradas < 8%, durante elas < 18%, total < 25%). `ai_squad_mm` segue
passando (< 60%).
- Limites: só locomoção no chão (sem giro no lugar, pivô de 180° ou start em ângulo com giro do corpo — próximos passos);
  sem estrafe da corrida nos clipes, não há direções da corrida; cada banco gera até 640 animações.

## Exemplos de Motion Matching (2026-11-01, `src/anim_mm_examples.inc`)
- Ferramentas > Pacotes de Animação > **Exemplos de Motion Matching**: 4 personagens no esqueleto do Mannequin, em
  `Content/Characters/MotionMatching/<Nome>`, cada um só com **idle, andar e corrida** (o Soldier também com 2 strafes)
  e um `ABP_<Nome>` cujo Locomotion é Motion Matching com ajustes próprios — o resto sai gerado no estilo de cada um:
  | Personagem | Clipes (cm/s) | Ajustes | Geradas |
  |---|---|---|---|
  | Brute (pesado, largo, passos lentos) | idle de joelho solto, andar 96, corrida 279 | peso 1,5, acomodação 0,6 | 4 starts, 16 stops, 4 trocas |
  | Runner (leve, joelhos altos, voo longo) | andar 147, corrida 540 | peso 0,7, inclinação 1,3 | 4 starts, 16 stops, 4 trocas |
  | Sneak (agachado o tempo todo) | idle agachado, andar 72, corrida 203 | inclinação 0,6, acomodação 0,15 | 4 starts, 16 stops, 4 trocas |
  | Soldier (ereto, braços junto ao corpo) | andar 121, corrida 403, strafes 100 | passo do ângulo 15° | 10 direções, 28 starts, 112 stops, 4 trocas |
- Para usar: Skeletal Mesh = `SK_<Nome>`, Anim Class = `ABP_<Nome>` num Character. O ABP só é criado se faltar (depois é
  do projeto); malha e clipes são refeitos ao gerar o pacote de novo.
- `make_abp` virou `abp_begin` (Event Graph com Speed/IsInAir, Foot IK, inclinação, cadeia de mira) + `abp_jump`;
  `strafe_pose` ganhou o tamanho do passo.
- Teste `mm_examples` (`--scene mm_examples`: os 4 lado a lado com o mesmo roteiro de andar/parar/correr/parar/trote/parar
  em suas velocidades): 13 starts e 30 stops tocados, geradas 4,3%, em jogo 16,2% (13,3% durante as geradas), 500 fps.
- As trocas de velocidade agora aceitam postura até 15% da perna (a corrida do Brute abaixa a pélvis).

## Motion Matching: clipes autorais, takes longos, giros e blend inteligente (2026-11-01, `src/anim_mm.inc`)
Pedido: usar também as animações feitas à mão (start, stop, turn 90/180) e **um take longo que faz tudo** (start walk,
start run, stop, turn, idle num arquivo só), achando os melhores momentos e espelhando quando servir.
- **Trajetória por quadro** (features 18): velocidade agora e em 0,3 s, **para onde estará virado em 0,27 s e 0,5 s**
  (cos/sen do giro), pés (posição/velocidade). A consulta usa a velocidade pedida e o giro pedido (para onde o Character
  quer ir, a 360°/s).
- **Root motion de qualquer clipe** (`mm_track_build`, por clipe e espelhado): o caminho e o giro do osso raiz; senão,
  clipe carregado pela pélvis (raiz parada, mocap) o caminho/giro da pélvis suavizados; senão um one-shot no lugar: a
  velocidade pelos pés plantados e o giro pela pélvis (giro no lugar). Sai da pose (`mm_unroot`: o corpo volta ao ator)
  e vira a trajetória de cada quadro; loops no lugar seguem constantes. Velocidade de clipe com root motion vem dela.
- **Takes longos**: todo quadro é candidato menos os últimos 0,3 s (sem futuro); o tempo segue a distância andada pelo
  corpo; chegando ao fim, busca outra coisa na hora. Espelhado também (giro para o outro lado, o outro pé).
- **Giro pelo clipe**: enquanto o quadro tocando gira (> 14°/s), o ator gira com ele (`CCharacter.anim_yaw_rate /
  anim_turning`) em vez de virar para a velocidade — giro no lugar e pivô de 180° sem o corpo "patinar".
- **Blend inteligente: inercialização** (no lugar do crossfade): na troca, a diferença entre a pose antiga e a nova (e a
  velocidade dela) decai numa mola criticamente amortecida em 0,25 s sobre a pose nova sozinha — nunca média de duas
  poses em fases diferentes. 4 amostras extras só durante a troca.
- **Os seus primeiro**: um one-shot seu que vai do repouso a uma velocidade (start) ou de uma velocidade ao repouso (stop)
  cobre a locomoção dessa velocidade/direção (±25%, ±30°): o gerador não faz esse start/stop; os gerados restantes têm um
  viés de custo (+0,12) quando há one-shots seus. Sem loops no banco (só um take), os limites do corpo saem das
  acelerações do próprio take.
- Editor: "Salvar como Animação" (com ou sem root motion) para a animação gerada mostrada; cada clipe mostra o tipo de
  root motion; o Depurador ganhou "Giro" e desenha para onde o quadro estará virado.
- Pacote de exemplos ganhou **LongTake** (`MM_LongTake`, 19,5 s, gravado do Motion Matching do Mannequin com caminho e giro
  na raiz: idle, andar, curva de 90° andando, correr, parar, pivô de 180°, trote, curva, parar; `ABP_LongTake` só com esse
  arquivo) e **Authored** (`Authored_StartWalk/_StopWalk/_StopRun` com root motion + idle/andar/trote/corrida).

| Medida | Resultado |
|---|---|
| `--scene mm_longtake` (banco = 1 arquivo; andar, curva 90°, parar, pivô 180°, correr, parar) | 32 trocas dentro do take, o clipe girou o ator 1,0-1,35 s (até 393°/s no pivô), deslize 24%, termina virado para onde correu, 441-454 fps |
| `--scene mm_authored` | 3-5 dos seus tocados; gerados só onde não há seu (stops gerados 24 → 8), deslize 16-17% |
| `mm_startstop` / `mm_examples` / `ai_squad_mm` | 17,4% / 15,8% / 29,3% (iguais a antes) |
| Banco do take longo | 1 170 quadros, 2,1 ms para montar |
| Busca (com features de giro) | ~0,009 ms; animação de 4 personagens 0,030 ms/quadro |
Testes `mm_longtake` e `mm_authored` (14 s cada).
- Limites: giro pedido assume a orientação para o movimento (360°/s); clipes de giro no lugar com a raiz parada dependem
  da pélvis girar; root motion vertical (pulos): seção abaixo.

## Motion Matching: root motion vertical, pulos no take (2026-11-20, `src/anim_mm.inc`)
Pedido: um take longo com pulos também (idle, andar, correr, pular); antes a altura do pulo ficava na pose (o boneco
subia, a cápsula não) e o Motion Matching podia escolher um quadro de pulo sozinho.
- **Trilha vertical** (`MmDb.tz`, junto da `trk`; `MmGroup.vert`): raiz no chão (a 25 cm do pé mais baixo, como na
  Unreal) que sobe mais de 8 cm → a altura dela; senão (raiz parada, no quadril ou no chão sem o pulo) os **voos**
  (`mm_air_arcs`): os dois pés 4 cm acima do mais baixo por 4 quadros ou mais, entre dois contatos → a pélvis acima da reta
  decolagem-aterrissagem (pico < 10 cm: o voo da corrida, fica na pose). Um one-shot parado com pulo vira `MM_TRK_FEET`.
- **Sai da pose** (`mm_unroot`: a raiz desce a altura) e é uma **feature**: velocidade vertical agora e em 0,3 s (índices 8
  e 9, peso como as velocidades; `ANIM_MM_FEAT` 18 → 20, pés agora de 10 a 19). No chão a consulta pede 0 → um quadro antes
  da decolagem custa ~50 e nunca é escolhido sem pedir. `MmDb.jump_fut`: a maior velocidade em 0,3 s de um quadro no chão.
- **Jump** (`char_jump`, também `bp_character_jump`): com `anim_jump_ok` (banco com pulos) pede a decolagem (`jump_wait`
  0,25 s; a consulta pede `jump_fut` em 0,3 s e a busca roda na hora); ninguém pegou → o pulo físico.
- **O clipe carrega a cápsula** (`mm_vertical`): começa só com o pedido, no chão, num quadro que sobe nos próximos 0,6 s;
  `CCharacter.anim_vz/anim_vertical` (no fim do componente, 124 de 128 bytes, `_Static_assert`) no lugar da gravidade
  (colide), mais uma mola para a altura do clipe (o atraso de um quadro); `p->vlock`: sem troca e em tempo real. Termina:
  aterrissou (o clipe de volta embaixo, ou a cápsula no chão descendo: degrau), bloqueou (20 cm fora: teto, beirada; a
  gravidade segue da velocidade do clipe, 0 no teto), fim do take, nunca saiu do chão em 1 s. Is Falling fica falso
  enquanto o clipe carrega (o estado Jump do ABP não corta).
- Fora do pulo carregado (caindo de uma beirada), a consulta pede a velocidade da cápsula e a gravidade: os quadros de
  queda do take servem de pose.
- Depurador: "Pulo: X cm/s para cima agora, Y em 0,3 s". Exemplo **JumpTake** (`MM_JumpTake`, 13,5 s, gravado como o
  LongTake com pulos parado, andando e correndo: JumpStart, voo balístico na raiz a 400 cm/s, Land; `ABP_JumpTake` com o
  estado Jump do `abp_jump`). Artigo `motion-matching-jumps`.

| Medida | Resultado |
|---|---|
| `--scene mm_jump` (4 Jumps: parado, andando, correndo, sob um teto a 30 cm) | 4 do take (3 aterrissaram, 1 bloqueado), nenhum sem pedir, cápsula subiu 82 cm, pélvis no máximo 98 cm sobre a base da cápsula (94 de pé), no chão no fim, 483 fps |
| Banco do JumpTake | 810 quadros, decolagem 351 cm/s, 1,4-3,3 ms para montar |
Teste `mm_jump` (14 s, área `animacao`).
- Limites: a aterrissagem num chão mais alto que o da animação é detectada, mas a pose fica com a diferença até o Foot IK;
  voos de mocap com a raiz parada supõem o chão plano dentro do voo; a previsão online (B13) faz o pulo físico.

## Curvas de morph (2026-11-09, sobra da Fase 1)
- **Na sequência** (`AnimSeq` cresceu no fim): até 64 curvas por nome, chaves (tempo, valor) lineares, ordenadas
  (`anim_curve_set/index/eval`). No `.ranim`: `"curves": [{ "name", "keys": [t, v, ...] }]`; arquivos antigos abrem sem.
- **Importação glTF**: o canal `weights` vira curvas (uma por morph target, pelo nome de `extras.targetNames`),
  amostradas a 30 fps com as corridas paradas cortadas (LINEAR, STEP e CUBICSPLINE pelo `gl_sample`). Malha com
  esqueleto: as curvas vão na sequência dele (`sk_write_anim`, `SkCurves`). Malha sem esqueleto (o Skeletal Mesh de um
  osso): uma sequência `A_<nó>_<animação>` de um osso parado com as curvas, e o ator passa a tocá-la. FBX: as blend
  shapes ainda não são importadas (sem curvas).
- **Avaliação**: as curvas andam com as poses numa tabela por thread indexada pelo buffer da pose (`PoseCurves`, 16):
  `sample_mapped` põe as da sequência (mapa por malha: o morph target de cada curva), `anim_blend` e `anim_blend_n`
  misturam (curva que falta conta 0), e o buffer final do `pose_one` vira `AnimInst.curve_w/curve_set`. Blend spaces,
  transições, montagens e máquinas de estado saem de graça. Sem curvas, um teste de `t_npc` por blend.
- **O peso**: `anim_morph_weight` = a curva da animação (se a pose a tem) > o do ator (Set Morph Target, Details) > o
  padrão do arquivo. A curva vence como na Unreal. No Anim Blueprint, Set/Get Morph Target sem Target pegam a malha
  (`morph_comp` sobe pelo CAttach).
- **Editor de sequência**: seção Curvas abaixo dos notifies: uma linha por curva com o gráfico (0..1, chaves em
  pontos), o valor no cursor (mudar põe uma chave ali), clique direito apaga a chave, x apaga a curva; Adicionar Curva
  com os morph targets da malha do esqueleto ou um nome digitado.
- **Teste `morph_targets`** (cresceu): um segundo glTF com a animação dos pesos (Wave 0→1, Bulge 0→0,5 em 1 s)
  importado: "1 animations", 2 curvas, 62 chaves, em 0,5 s Wave 0,500 e Bulge 0,250; jogando, o ator segue as curvas por
  cima de um Set Morph Target 0,9 (Wave 0,169, Bulge 0,084: metade); curva autoral gravada no `.ranim`.
- **Medidas**: 8,6–9,4 ns por valor de curva (31 chaves, busca binária); `anim_update` com o ator de curvas 0,016–0,022 ms.

## Reimportar e aviso de formato antigo (2026-11-10, "de novo o vestido dela" no MyProject)
- A Delight do MyProject era de 05/11 (`.rskm` v1, partes rígidas): o skinning suave só vale reimportando.
- `.rskm` ganhou `"source"` (o arquivo importado: Tlender, glTF, FBX) e `"rigid"` (o auto rig, partes de propósito).
- **Reimportar** (Content Browser, botão direito num Skeletal Mesh; ou o botão no Details): lê a origem de novo para a
  mesma pasta em segundo plano; sem origem conhecida, pergunta o arquivo. Ao terminar, `anim_reload_folder` relê os
  Skeletal Meshes daquela pasta no lugar e os atores refazem partes/peles (`--reimport <vpath>` testa).
- `anim_mesh_old_parts`: partes cortadas de malha importada, sem peles, sem `rigid`: o Details mostra o aviso.
- Teste manual na cópia do MyProject: 124 partes -> 5 peles, vestido inteiro.

## Realtime no editor (2026-11-10)
- Menu ☰ do viewport > Tempo Real (Ctrl+R, ligado por padrão): cada Skeletal Mesh com animação própria toca em loop
  sem Play (`anim_set_editor_realtime`); desligado, partículas e decals também param.

## Vestido da Delight inflado depois do Reimportar (2026-11-10)
- A shape key "Hide" do vestido foi salva no `.tlend` com valor -1 e faixa 0..1: o Tlender (e o Blender) a avalia presa
  na faixa (`shapekey.c`, 0 aí); o importador usava -1 cru e o morph empurrava 1 226 vértices para fora, um cone
  cobrindo o corpo até o pescoço (na pose de ligação também). Agora o peso padrão é `clamp(valor, min, max)` e 0 se
  muda. Personagens já importados: Reimportar.

## Sockets no jogo (Q10, 2026-11-18)
O que o Q7 deixou para depois: o anexo no socket com o deslocamento dele. `anim_attach_to_socket` (componente: CAttach
`ATTACH_BONE`) e `anim_attach_actor_to_socket` (um ator) entram numa tabela que `follow_assign` distribui pelas instâncias
na thread principal; no `pose_one`, depois do ragdoll e dos ossos dinâmicos, `place_followers` põe cada um em osso ×
socket × o seu deslocamento. `anim_socket_world` dá o lugar de um socket ou osso na última pose (guardada só para quem
pediu). O inventário equipa assim. Nós Attach Actor To Socket, Detach From Socket, Get Socket Location / Rotation;
teste `socket_offset`. Detalhes em [ikrig.md](ikrig.md).

## Estilos de locomoção (Q27.11, 2026-11-23: os arquétipos de movimento do GTA 6)
- **Set Locomotion Style** / **Get Locomotion Style** (`anim_set_locomotion_style`): Normal, Injured, Tired, Drunk,
  Carrying por personagem (`AnimInst.style`).
- **No Motion Matching**: cada grupo do banco ganha o estilo pelo nome do clipe (injur/limp, tired, drunk, carry; o resto
  Normal) e o banco guarda quais tem (`MmDb.styles`). A busca (`mm_search`) só olha os grupos do estilo pedido quando o
  banco tem algum; senão os Normais. Um grupo de outro estilo tocando tem o custo atual infinito: troca já, pela
  transição de 0,2 s de sempre.
- **Camada procedural** (sem clipes do estilo): depois do grafo, antes do `anim_world`, alguns ossos (pelve, 2 da
  coluna, cabeça, braços, antebraços, panturrilhas; achados pelo nome uma vez por malha) giram no espaço da malha em
  volta dos eixos do personagem (a frente pelo pé → dedos no bind; o giro aplicado no referencial do pai:
  inv(P)·dq·P). O peso de cada estilo segue o pedido por uma mola criticamente amortecida (k 7: ~0,6 s, peso e
  velocidade contínuos: sem salto). Injured também sobe pelo `phys_limp` do Ragdoll Ativo (Q27.5).
- **Medidas** (RTX 3060, cena `loco_styles`: o Mannequin no ABP_Mannequin_MM andando, estilo trocado a cada 2 s):
  cabeça sobre a pelve a no máximo 40-70 cm/s andando e 57-71 cm/s numa troca (sem salto); Carregando: a mão 31,7 cm
  mais alta; **a camada 0,0006 ms por personagem** (meta 0,01). O curvado do Cansado não aparece na cabeça medida: a
  cadeia de mira do ABP reorienta a cabeça e a coluna depois da camada (com o eixo invertido mediu o mesmo).
- Fica para depois: motion warping até alvos (porta, cobertura, maçaneta), tabelas de transição por arquétipo, a
  velocidade de cada estilo (hoje o Character anda na mesma), a camada antes da cadeia de mira.

## Root motion, Motion Warping e montages pareados (2026-11-23, `src/anim_warp.inc`)
Pedido do usuário: "dois personagens devam tocar a mesma animação em uma posição específica um em relação ao outro, um
sendo o centro, em sincronia" e "algo igual ao motion warping da Unreal".
- **Root Motion do montage** (`AnimMontage.root_motion`, caixa no Montage Editor): o caminho da raiz é tirado do clipe a
  30 Hz como o `mm_track_build` do MM (`RmTrack`, cache por sequência × malha: modo raiz quando o `root` anda, modo pelve
  quando só a pelve anda; yaw relativo suavizado). A pose perde esse caminho (`rm_inplace`, nos workers, só leitura): no
  modo raiz o osso 0 volta ao lugar e ao giro do 1º quadro; no modo pelve a pelve é corrigida pelo referencial da raiz.
  O delta de cada passo (`rm_step`, no `step_logic` do montage) vai para o ator: um Character pega numa tabela pendente
  (`anim_root_motion_take`) no `move_character` (colisão, sonda do chão, gravidade quando não sobe, input zerado); outro
  ator anda direto. Root motion inteiro no blend in e no blend out natural (o deslocamento do clipe e o alvo do warp
  exatos); só um Stop Montage antes do fim (`mcut`) apaga com o peso. No fim natural a velocidade horizontal zera (o
  Orient Rotation to Movement daqui segue a velocidade: sem isso ele girava 33° deslizando).
- **Motion Warping** (`AnimWarpWindow` × 4 por montage: alvo por nome, início/fim, Lugar/Direção/Ignorar Altura): a
  correção (o que falta até o alvo − o que o clipe ainda anda até o fim da janela) é distribuída pela fração do caminho
  do próprio clipe naquele passo (parado: pela fração do tempo); o giro pela fração do tempo. O último passo da janela
  fecha o resto: chega exato. Alvos por ator e nome (`anim_warp_target_set`/`_remove`), nós **Add or Update Warp
  Target** e **Remove Warp Target**.
- **Montage pareado** (`partner`, `partner_offset` cm frente/direita/cima, `partner_yaw`, `partner_align` s): **Play
  Paired Montage** toca os dois no mesmo quadro; `pairs_update` (depois do passo lógico) trava o tempo do parceiro no
  do primário e o põe em P(t)·Rp(t)⁻¹·Offset·Rs(t) (P o ator primário, Rp/Rs os caminhos de raiz dos dois clipes),
  entrando por smoothstep no Align Time a partir de onde estava; a velocidade e o input do parceiro zeram. Acaba quando
  o do primário acaba; **Stop Paired Montage** encerra os dois por qualquer um.
- **Quadros longos** (achado com `RE_FIXED_DT=0.05`: o root motion andava 172,5 de 180, o warp errava 10 cm, o par 12-24
  cm): o blend out do fim natural tirava 1/bout de peso por quadro e o montage acabava antes de o tempo chegar ao fim do
  clipe (o último passo, e o fechamento da janela de warp, nunca rodavam). Agora o peso do fim natural é o tempo que
  falta / blend out e o montage só acaba no fim do clipe; o par coloca o parceiro uma última vez no quadro em que o
  primário acaba. Com 16, 33, 50 e 80 ms por quadro: 180,0 cm, warp 0,0 cm, par 0,0 cm. `RE_FIXED_DT=s` (main.c) deixa
  qualquer cena com quadros de s segundos, igual em toda execução.
- Montage JSON: `root_motion`, `warps[{target,start,end,flags}]`, `partner`, `partner_x/y/z`, `partner_yaw`,
  `partner_align` (sem as chaves: os padrões antigos, nada muda nos montages que já existiam).
- **Medidas** (RTX 3060, cena `warp`, teste `warp` na área animacao: dois Mannequins, um clipe feito pelo teste com a
  raiz andando 150 cm/s por 1,2 s): root motion levou o ator 181,8 cm (o clipe: 180), a raiz da malha a 0,0 cm do ator no
  meio; o warp para um alvo 250 cm à frente e 200 ao lado, de frente para 90°, terminou a 0,0 cm e 0,0°; o par: o tempo
  do parceiro igual ao do primário (0,0000 s) e ele terminou a 0,1 cm e 0,0° do lugar. A animação inteira dos dois:
  0,012 ms por quadro.
- Fica para depois (lista mostrada ao usuário; feitos em seguida, seções abaixo): traversal (pular/subir/escalar com trace + warp), Orientation e Stride
  Warping na locomoção, reações de golpe por direção, escolha da variante do par pelo ângulo de entrada (Contextual
  Animation), janelas de combo/ataque no montage, cobertura.

## Traversal (2026-11-23, `src/anim_traversal.inc`)
Como o traversal do Game Animation Sample da Unreal: **Try Traversal** (Target, Max Distance, Max Height, Draw Debug →
Return Value, Action, Height).
- **Checagem** (`trav_check`, ~15 traces de `sq_trace`): o chão sob o Character; linhas à frente em 6 alturas (35..210
  cm) acham a face mais perto (normal quase horizontal); uma linha de cima para baixo 6 cm dentro dela dá o topo; passos
  de 10 cm para dentro até o topo acabar dão a profundidade e a borda de trás; o chão 45 cm além dela; cápsula do
  Character no ponto de chegada (espaço). **Vault**: altura ≤ 130, profundidade ≤ 90, chão atrás 40 cm abaixo do topo e
  espaço lá; senão **Mantle** (≤ 130) ou **Climb** (≤ Max Height, 230) se a profundidade ≥ 50 e há espaço em cima;
  senão None (degrau < 30 cm, alto demais, sem espaço).
- **Clipes** (`MM_Traversal_Vault/Mantle/Climb`, em `Characters/Mannequin/Traversal`, feitos quando faltam ou são de
  geração antiga): poses-chave como as do get-up (`gk_pose`: pelve, coluna, cabeça e mãos/pés por IK de dois ossos com
  polo) com o lugar da raiz em cada chave (`TKey.root`): as chaves no espaço do clipe menos a raiz, e a raiz escrita no
  osso `root` (o root motion modo 1, com altura). Cada um contra um obstáculo de referência (Vault: frente a 60, 100 de
  altura, 30 de fundo; Mantle: 50 / 100; Climb: 45 / 180).
- **Montages** `AM_Traversal_*`: Root Motion, **Ignorar Colisão** (novo `AnimMontage.no_collision`: o `move_character`
  soma o delta sem colidir e assenta no chão no fim, como o modo Flying), janela `FrontLedge` (0 até as mãos na borda:
  lugar e direção, com altura) e `BackFloor`/`TopFloor` (do empurrão ao fim: lugar com altura). Os alvos: a borda achada
  mais o deslocamento da raiz do clipe em relação à borda de referência naquele instante (e a mesma altura acima do chão
  que o Character tinha); a direção, de frente para a face.
- O template **Terceira Pessoa**: Espaço → Try Traversal → Branch, False → Jump (`template_traversal_on_jump`).
- **Medidas** (RTX 3060, cena `traversal`, teste `traversal` na área animacao: quatro Mannequins a 90 cm de um muro de
  95×30, um bloco 95×300, um bloco 175×300 e um muro de 400): Vault, Mantle, Climb e None certos; terminam no chão atrás
  (z 89,0 de 89,0) e em cima (183,1 e 263,1 = topo + 89 − 1); as mãos a 0,6-0,9 cm da altura da borda e 0,1-0,3 cm do
  lugar delas no clipe enquanto apoiam; **0,044 ms por checagem**. O teste mede pelo tempo do montage (a pose de quem
  está longe é amostrada menos vezes: medir pelo relógio da cena errava 70 cm).

## Orientation e Stride Warping (2026-11-23, `anim_nodes.inc`)
- Nós `AN_ORIENT_WARP` e `AN_STRIDE_WARP` (Skeletal Controls). O passo lógico guarda o movimento do Character (ângulo da
  velocidade contra a frente, velocidade) em `AnimInst.mv_angle/mv_speed`.
- **Orientation**: a pelve (bone[0]) gira para o ângulo (limitado, suavizado por Velocidade do Giro; para trás vira o
  giro curto; Quatro Direções: só o resto até o múltiplo de 90) e a coluna (bone[1..3]) devolve o giro (`turn_bones`).
- **Stride**: a velocidade da animação vem do pé apoiado (o mais baixo) deslizando para trás no espaço do ator, suavizada;
  a escala = velocidade do Character / a da animação (limitada); cada pé vai para seu ponto ao longo do movimento a partir
  do quadril vezes a escala (`two_bone_to`), a pelve desce nas passadas longas.
- Teste `anim_warping` (os nós postos em memória no ABP_Mannequin): a 45° da frente, quadril 46,5° e peito -0,2°; sem o
  warp -0,9° e 0,1°; velocidade lida da animação 143 contra 150 cm/s; passada forçada em 1,6: os pés abrem 1,48×.
- **Variante do par e Can Play** (2026-11-23, `anim_traversal.inc`): `pair_fit` = distância do parceiro ao lugar dele no
  montage (a partir do primário no começo) + 2 cm por grau de giro, dentro dos limites; Find Paired Montage escolhe o
  menor entre os montages com parceiro e o prefixo; Can Play soma uma linha na altura do peito sem acerto entre os dois.
  Teste no `warp`: parceiro atrás → AM_PairBack (Can Play do A falso, do Back verdadeiro); na frente → AM_PairA.

## Janelas de combo e de acerto (2026-11-23, `anim_combo.inc`)
- `AnimMontage.windows` (até 8: nome, início, fim, tipo State/Combo/Hit, Próxima Seção, osso e raio), no JSON do montage
  ("windows") e no Montage Editor (seção Janelas). `combo_step` roda no passo do montage depois do root motion: abre e
  fecha as janelas (Event On Montage Window), o Combo encadeia no fim da janela se houve Montage Combo Input dentro dela
  (ou 0,15 s antes; senão o montage sai no fim da seção: `mcut` com o blend out), o Hit varre uma esfera do lugar do osso
  no quadro anterior ao de agora (`sq_trace_multi`, o próprio ator fora) e chama Event On Montage Hit uma vez por ator
  por janela. Estado por ator numa tabela (64), zerada no fim do Play.
- Teste `combo`: sem entrada termina depois do Attack1 e o boneco à frente é atingido 1 vez; aperto antes da janela
  recusado; Is In Montage Window vê "Chain"; aperto dentro encadeia no Attack2.

## Reação ao golpe pela direção (2026-11-23, `anim.c flinch_apply`)
- **Play Hit Reaction** (`anim_hit_reaction`): o lado pelo lugar do golpe contra a frente do ator (Front/Back/Left/Right),
  um impulso numa mola subamortecida (k 16, amortecimento 0,45) e, com um prefixo, o montage `<prefixo>_<lado>` (ou
  `_Front`). A camada roda depois da de estilos: coluna (2 ossos), pescoço e cabeça giram em volta de up × direção do
  empurrão, levada do mundo para o espaço da malha pelas linhas do ator (a frente suposta +Y dos estilos não serve ao
  Mannequin da engine, que olha para +X: a primeira versão jogava a cabeça para o lado).
- Teste no `combo`: soco pela frente → cabeça 10,8 cm para trás; pela direita → 9,8 cm para a esquerda; de volta em 1,3 s.

## Cobertura (2026-11-23, `anim_cover.inc`)
- **Enter Cover**: 16 linhas em volta na altura do peito (70 cm do pé) acham a face mais perto; o lugar = o ponto + a
  normal × (raio + 2); baixa quando uma linha a 125 cm em direção à parede não bate. Estado por Character (64).
- **Movimento** (`move_character`): antes, `anim_cover_input` projeta o input na tangente (n × up), zera para o lado de uma
  ponta (uma linha para dentro da parede meio raio + 15 cm ao lado não bate) e desacelera (0,7; baixa 0,55); depois,
  `anim_cover_after` desliza até o lugar em 0,3 s, segura a distância da parede e vira de costas para ela.
- **Pose** (depois da reação ao golpe): baixa → pelve 40 cm para baixo com os pés presos pelo IK de dois ossos; espiar
  numa ponta → coluna inclinada ~20° para o lado aberto (em volta da frente levada ao espaço da malha).
- Teste `cover`: H a 149,0 cm do plano da parede (o esperado) virado para 180°; L agachado (pelve 40 cm abaixo); H para
  na ponta direita; espiando inclina 16,6 cm para fora; Exit Cover solta. Print conferido (joelhos para a frente).


## Peles em massa: rígidas, LODs, URO (item 4 parte 2, 2026-11-24/25)
- **LODs de pele com bordas livres e sloppy** (`mesh_build.c` `t_free_borders`): a cápsula da Unamed (bordas de UV e de
  normal travavam a simplificação: parava em 20757 de 27852 índices) ganha LODs pelo `meshopt_simplifyWithAttributes` sem
  travar bordas e, se ainda passar de 70%, pelo `meshopt_simplifySloppy`: 9284 -> 2241 (7,63 cm) -> 381 (20,72) -> 255
  (245,19). Cada LOD só entra com o erro abaixo de ~1 px. Conferir: `rivora --skin-lods-test <arquivo.rmesh>`; uma pele de
  4096 triângulos ou mais que fica sem LODs avisa no log.
- **URO por distância** (`anim.c`, `REALENGINE_NO_URO`): a pose atualiza todo quadro até 15 m, depois a cada 2, 4 e 8
  quadros (40 e 80 m), pela câmera do último quadro (`render_last_camera`).
- **Pele rígida** e **dormente**: notas-dev/escala.md "Milhares de Skeletal Meshes".
- Testes: `skin_5000` (5000 animadas, 87 fps, 0 sem área), `skin_5000_still` (5000 paradas e dormentes, 143 fps),
  `skin_still`, `skin_noroom`, `skin_gpu`, `skin_dag` (abaixo).

## Nanite na pele viva (item 4 passo 3, 2026-11-25)
- Uma Skeletal Mesh densa (Auto: 16 384 triângulos; On no `.rlod`: 512) ganha o DAG de clusters como as estáticas, e ele
  serve também as instâncias animadas: `mesh.vert` já lê os vértices dobrados pelo índice local do LOD 0, igual para os
  clusters. Em `cull.comp` (passe 6) as esferas dos clusters de uma pele viva crescem 35% do raio da pose de ligação (a
  mesma folga da esfera da instância, `upload_mesh_info`), dos dois lados de cada grupo: o corte continua único.
- Fora: Masked (fica inteira), soft body (vértices próprios), Nanite Tessellation (dobraria a pose de ligação).
- Teste `skin_dag`: o rig subdividido na importação (`REALENGINE_IMPORT_SUBDIV=4`: 236 -> 60 416 triângulos), 9 cópias
  dobradas, validado, `REALENGINE_DAG_STATS` mostra as instâncias pelo DAG.- Rodada 3 (25/11): `skin_dag` passa (9 instâncias pelo DAG, 291 924 vértices dobrados, validado, 321 fps); o rig denso a
  1,8 m da câmera com o DAG (6 instâncias, 17 617 triângulos) e sem ele: a mesma imagem, sem buracos nem rachaduras, 349
  e 358 fps (o custo igual: o rig é pequeno na tela).