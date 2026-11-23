# Auditoria das ferramentas de artista (Q23, lote 8 do plano 0.43, 18/11/2026)

Pedido: comparar a Real Engine com a Unreal 5.8 nas ferramentas de artista, deixar o próximo plano (0.44) pronto e
corrigir o que fosse pequeno e quebrado. Conferido no código (branch do lote 7, `2669c85`), não só nas notas.
Tamanho: **P** até 1 dia, **M** 2-4 dias, **G** 5+ dias (um subchat). Prioridade pelo que um artista esbarra primeiro.

## 1. O que existe e o que falta, por área

### 1.1 Modelagem leve (Modeling Mode da Unreal, Shift+5)
- **Existe**: formas básicas (Cubo, Esfera, Cilindro, Cone, Plano: `BasicShape`, `components.h`; Adicionar Rápido em
  `editor.c` `quick_add_items`); blockout por caixas (`proto.c`: Caixa, Escada, Rampa, Parede com porta, Sala, Corredor,
  Plataforma, alças na viewport; [prototipagem.md](prototipagem.md)); LODs automáticos pelo meshoptimizer
  (`mesh_build.c` `make_lods`, Criar > LODs do Content Browser em `content_create.c`); UV2 de lightmap uma célula por
  triângulo (`mesh_build.h`); layout de UV só para ver (viewport do asset, canal 0). Modos do editor: só Seleção,
  Paisagem e Folhagem (`ed->land_tool`, `ed->foliage_mode`; Shift+1/2/3); Shift+4/5 não fazem nada.
- **Falta**: o Modeling Mode inteiro — primitivas com parâmetros (toro, cápsula, disco, seta, escada/rampa de malha de
  verdade), PolyEdit (extrudar, inset, bevel, ponte, apagar faces), PolyGroups, booleanas, remesh/simplificar à mão,
  espelhar, ferramentas de normais, UV de verdade (desdobrar, layout, editor de UV), **pivô** (o gizmo sempre na origem
  do ator: sem Alt+meio arrastar nem Set Pivot), **Merge Actors** / Converter em Static Mesh (só o HLOD junta malhas),
  blockout (`proto`) virar malha.
- **Prioridade**: Merge Actors + pivô + blockout→malha **alta (P/M)**; primitivas + PolyEdit **média (G)**; booleanas e
  desdobrar UV **baixa (G)**: o caminho do artista é modelar no Tlender e importar (Q19).

### 1.2 Pintura (Mesh Paint, Shift+4)
- **Existe**: pincéis só no Landscape (escultura e camadas, `mode_panel` `editor.c`) e na Folhagem. Cor por vértice é
  guardada (`RMESH_COLORS`), desenhada e lida pelo nó Color Attribute (M5), mas só vem do glTF `COLOR_0`. O soft body
  "Pintado" e o tecido leem pesos que precisam vir pintados de fora; o pin do tecido é por faixa/preset (`cloth.c`).
- **Falta**: pintar cor por vértice na malha (por instância e no asset), pintar textura, **pintar pesos** (skin — hoje
  a skin é rígida, um osso por parte; pin do tecido; amarração do soft body), queda do pincel (falloff) até na Paisagem.
  [materiais.md](materiais.md) já listava "pintar a cor por vértice no editor".
- **Prioridade**: cor por vértice + pesos do tecido/soft body **alta (M)**; pintura de textura **baixa (G)**.

### 1.3 Editor de texturas (Texture Editor)
- **Existe**: visualizador (`asset_viewer.c` `draw_texture`): zoom/pan, Ajustar/1:1, canais R/G/B/A, valor do pixel,
  tamanho, KB no disco, tem alfa. Compressão global BC7 por material assado (`material_gpu.inc` `bc7_on`), UDIM BC7/BC5
  (Q18), streaming por material.
- **Corrigido agora (Q23, abaixo)**: `.rudim`, `.hdr`, TIFF/GIF, `.exr` sem janela, mips, exposição, seguir o arquivo.
- **Falta**: um asset de textura com configurações (hoje a textura é o arquivo-fonte): sRGB, compressão por textura
  (BC1/3/4/5/6H/7, sem compressão), tamanho máximo, LOD bias, geração de mips, grupo; ajustes (brilho, saturação,
  matiz, curvas, inverter, alfa); memória na GPU; OpenEXR; reimportar; HDR acima de 8 bits nas texturas (hoje o .hdr
  vira 8 bits sRGB em 0 EV).
- **Prioridade**: **média (M)**. Cuidado: os materiais assam o grafo em texturas ([materiais.md](materiais.md)); as
  configurações valem no bake (tamanho, sRGB, compressão do resultado), não na amostragem do jogo.

### 1.4 Retarget de animação (IK Retargeter)
- **Existe**: nada com esse nome. O clipe toca em qualquer esqueleto com os mesmos nomes de ossos (`anim.c`
  `track_map`, `strcmp`), sem corrigir proporções nem pose de descanso (os Mannequins Male/Female dependem disso).
  `ikrig.c` `bone_like`/`ikrig_create` já acham mãos, pés, cabeça e bacia pelos nomes.
- **Falta**: cadeias de retarget e raiz no IK Rig; o asset IK Retargeter (rig de origem e de destino, mapeamento
  automático por nome, pose de retarget editável, FK por rotação, metas de IK nas pontas, escala da bacia/root motion);
  prévia lado a lado; exportar em lote; "usar um esqueleto existente" no import (`sk_write_anim` sempre grava contra o
  esqueleto importado). Os editores filtram animações pelo **nome** do esqueleto, mas o jogo toca pelo nome dos ossos:
  o editor do `SK_Mannequin_Male` não lista os `MM_*` que tocam nele (`anim_editor.c`).
- **Prioridade**: **alta (G)** — é o que destrava usar animações compradas/capturadas em personagens próprios.

### 1.5 Control Rig
- **Existe**: nós de controle no Anim Graph (Two Bone IK, Leg IK, Aim Offset, Lean, Transform (Modify) Bone, Look At,
  Mirror: `anim.h`, `anim_nodes.inc`) e o IK Rig (Chain/FABRIK, Limb, Full Body, Look At; metas por nó ou por ator).
  Control rigs do Blender/Tlender são assados por quadro no import (`tlend_import.c`).
- **Falta**: o Control Rig todo — grafo de rig (bytecode, como o dos Blueprints), controles com formas no viewport,
  FK/IK com troca e espaços, solve para frente/para trás, trilha de Control Rig no Sequencer (animar à mão), nó Control
  Rig no Anim Graph e em jogo. O IK Rig roda depois do grafo inteiro (`anim.c`), não como nó.
- **Prioridade**: **média (G, o maior item)**: depende da edição de pose (1.6) e do Sequencer com ossos.

### 1.6 Editores de animação (sequência, Montage, Blend Space, Sequencer)
- **Existe**: janela "persona" com abas Modelo/Esqueleto/Animação/Física/IK Rig (`anim_editor.c`), linha do tempo com
  notifies, curvas de morph (lineares), Montage (blend, rate, slot, seções), Blend Space (`anim_graph_ui.inc`), Motion
  Matching; Sequencer com Transform/Visibilidade/Luz/FOV/Animação/Cortes/Evento/Áudio/Fade/Terror e Editor de Curvas
  com tangentes ([sequencer.md](sequencer.md)).
- **Falta**: **editar pose e pôr chave em ossos** (gizmo de osso, Animation Mode, "Edit in Sequencer", assar para Anim
  Sequence, trilha de ossos no Sequencer); a trilha de Animação **não mostra a pose ao arrastar o cursor** no editor
  (`sequencer_ui.c` avalia sem eventos; a trilha só age com eventos) nem tem opções (blend in/out, início, rate, peso);
  root motion na sequência (só no Motion Matching), aditivo com pose base escolhida, compressão, curvas genéricas e o nó
  Get Curve Value, notify states, Pose Asset, Blend Space como asset próprio (hoje cria um Anim Blueprint; 9 amostras);
  régua com números, zoom e passo por quadro. **Seções do Montage não fazem nada em jogo** (salvas, mas `anim_play_montage`
  não tem seção e não há Jump to Section); slots do Montage fixos (DefaultSlot/UpperBody).
- **Prioridade**: seções do Montage **alta (P)**; prévia da trilha de Animação + edição de pose e chaves **alta (M)**;
  configurações da sequência, curvas, Pose Asset **média (M)**.

### 1.7 Esqueleto
- **Existe**: o esqueleto é o Skeletal Mesh (sem asset Skeleton); árvore de ossos, sockets (adicionar, renomear,
  re-parentar, posição/rotação, malha de prévia, apagar; gizmo só de mover; 32), sockets em jogo.
- **Falta**: adicionar/remover/renomear ossos, ossos virtuais, esqueletos compatíveis, escala e gizmo de rotação do
  socket. **Risco**: editar a Bind Pose pelos números (`anim_editor.c`) não reconstrói `bind_world/bind_inv` (só no
  carregamento): numa malha com skin, salvar e reabrir pode deslocar a pele (não reproduzido).
- **Prioridade**: **média (P/M)**; conferir o risco da Bind Pose primeiro.

### 1.8 Static Mesh Editor
- **Existe**: um visualizador (`asset_viewer.c` `draw_mesh` + o viewport do Q7): triângulos, vértices, tamanho, grade,
  material de prévia, LODs (só leitura), ms de CPU/GPU, Enquadrar; LOD em DAG estilo Nanite (`mesh_clod.cpp`).
- **Falta**: tudo o que é configuração do asset — número de LODs, redução %, tamanhos de tela, regenerar quando já tem
  LODs (hoje só gera se não tem, e recusa malhas com cor/morph/skin), LODs importados; **colisão simples no asset**
  (caixa, esfera, cápsula, k-DOP, convexo automático, complexa como simples — hoje a colisão é por ator, `components.h`);
  sockets de Static Mesh; **slots de material** (o componente tem um material; o import separa a malha por material);
  o material de prévia não é salvo como padrão; resolução de lightmap, Nanite por malha, pivô, recalcular
  normais/tangentes, canal de UV (fixo no 0).
- **Prioridade**: **alta (M)**.

### 1.9 Material Editor
- **Existe**: canvas com paleta e busca (Shift+A), atalhos 1/3/T/M/A/L/U, reroute, Details (blend, two-sided, refração,
  transmissão, domínio Surface/Light Function, Default/Eye/Toon, tesselação), estatística (instruções, parâmetros, ms de
  bake), prévia ao vivo pela GPU, Material Instance (escalar e vetor). Os nós do Blender/Tlender; o grafo é assado em
  texturas (2-3 leituras por pixel no jogo).
- **Falta**: **desfazer/refazer, copiar/colar/duplicar nós, caixas de comentário**; Material Functions (subgrafos);
  Material Layers; nó Custom (GLSL, no bake); Texture Parameter e Static Switch; grupos e ordem de parâmetros; instância
  de instância na UI; contagem de samplers; curvas (RGB/Vector/Float). Vários inputs do Principled ainda "sem efeito"
  (`mat_input_has_effect`).
- **Prioridade**: desfazer/copiar/comentário **alta (P/M)**; Functions + Texture Parameter **média (M)**; Layers baixa.

### 1.10 Partículas (Niagara)
- **Existe**: editor no layout do Niagara (`particle_editor.c`: pilha de módulos, emissores, curvas, linha do tempo,
  prévia no nível), 25 módulos (Ribbon, Mesh, SubUV, Collision, Curl, Kill Box...), GPU determinística, fogo em grade.
- **Falta**: módulos próprios (scratch pad), parâmetros de usuário, data interfaces, eventos (morte, colisão) e
  sub-emissores, escalabilidade/LOD por distância, editor de curvas livre (as curvas têm pontos fixos), desfazer;
  partículas suaves, ordenação, colisão pela profundidade, sombra das malhas ([particulas.md](particulas.md)).
- **Prioridade**: eventos/sub-emissores + escalabilidade **média (M)**; scratch pad baixa (G).

### 1.11 Conveniências do editor
- **Existe**: miniaturas reais (texturas, malhas, animações, materiais), reimportar Skeletal Mesh, Content Browser com
  renomear/apagar/Explorer/LFS/Criar, encaixe em grade/rotação/escala, Snap to Floor.
- **Falta**: reimportar Static Mesh/textura/material; Find References, Reference Viewer, Size Map, Duplicate, Migrate;
  edição de vários objetos no Details (Property Matrix); encaixe em superfície/vértice, alinhar e distribuir; desfazer
  operações de arquivo; coleções/favoritos e filtro por tipo.
- **Prioridade**: Reimport + Find References + Duplicate **alta (P/M)**; edição múltipla **média (M)**.

## 2. Corrigido agora (Q23)
Pequenas, quebradas ou óbvias; cada uma com teste.
1. **`.rudim` não abria no visualizador** (a extensão faltava em `asset_viewer_handles`): abre o 1º tile.
2. **`.hdr` listado mas nunca lido** (o WIC não decodifica Radiance): decodificador RGBE próprio em `mat_image.c`
   (`mat_hdr_decode`, linhas planas e RLE novo/antigo) → a miniatura, o visualizador e os materiais leem o .hdr (8 bits
   sRGB em 0 EV). No visualizador: **Exposição HDR** (±8 EV, só a vista).
3. **`.exr` (e qualquer imagem ilegível) abria nada** (sem janela, como se o clique falhasse): a janela abre e diz o
   porquê ("OpenEXR ainda não é lido: salve como .hdr ou PNG/TGA").
4. **TIFF/GIF** abriam no visualizador mas o Content Browser não os tratava como textura (`content.c` `kind_of`).
5. **Nível de Mip** no visualizador (a cadeia inteira até 1×1, o tamanho de cada um), como o Mip Level da Unreal.
6. **Miniaturas e visualizador seguem o arquivo**: antes a miniatura ficava a velha até reiniciar o editor (pintou no
   Photoshop, salvou, nada mudava). O worker das miniaturas, ocioso, olha a data/tamanho de um arquivo a cada 15 ms e
   refaz a miniatura que mudou (a velha fica desenhada até a nova subir); o visualizador aberto confere o seu a cada 0,5 s
   e relê (mantendo zoom, canais e mip).
7. **Vértices do Static Mesh errados**: era o intervalo dos índices (contava vértices de outras malhas empacotadas entre
   eles e os sem triângulo); agora os usados, cada um uma vez (`asset_mesh_used_verts`).
8. **Sequencer deixava o personagem preso**: a trilha de Animação troca o ator para o modo Animação (`anim_play` tira o
   Anim BP) e nada o devolvia (`anim_use_blueprint` nunca era chamado): depois de uma cutscene o Character ficava parado
   no último clipe. Agora, ao acabar (ou Stop Sequence), quem tem Anim BP volta a ele (o Restore State da seção da
   Unreal); sem Anim BP fica no último quadro, como antes. `anim_uses_blueprint` novo; `SeqStats.anim_restored`.
9. **Montage**: "Criar Montage" gravava sempre em `/Content/Characters/Mannequin`; agora ao lado da sequência. Seções
   ganham **Remover Seção** (só dava para adicionar). "Adicionar Notify" com 32 notifies ficava mudo: agora desabilitado
   com o aviso.
10. Notas que mentiam: modos do editor (Folhagem), camadas/desfazer/heightmap da Paisagem (feitos), Material Instance
    pelo clique direito, BC7/Masked/Ribbon "faltando", miniaturas "faltando", prévia 2D do IK Rig.

### Medidas (RTX 3060, 2560×1417, GPU dividida com dois editores de outras sessões; pares alternados base `2669c85` × Q23)
| O quê | Antes | Depois |
|---|---|---|
| .hdr 2048×1024 RLE | não abria (miniatura falha, janela nenhuma) | decodificado em **18-19 ms** (8 MB) |
| Miniatura de textura editada fora | velha até reiniciar | refeita em **~6 quadros**; **0 µs** na thread principal (o stat no worker) |
| Visualizador aberto, arquivo mudou | velho | relido em < 0,5 s (um stat a cada 0,5 s na thread principal) |
| Vértices do teste (9 índices em 3 malhas) | 13 (o intervalo) | **7** (os usados); 3 M índices em **10-11 ms** (uma vez ao abrir) |
| Personagem depois da cutscene | Anim BP desligado para sempre | volta ao Anim BP (1 restaurado no teste) |
| Editor vazio (`--bench 3`), 2 pares | 279 / 249 fps | 457 / 171 fps (mesmo nível: a GPU dividida) |
| `--seq-test --bench 3`, 2 pares | 357 / 285 fps | 317 / 269 fps (a cena do teste ganhou um Mannequin com Anim BP) |
A 1ª versão olhava um arquivo por quadro na thread principal: **119 µs/quadro** (o `GetFileAttributesExW` nesta pasta,
com antivírus/OneDrive) — por isso foi para o worker.

### Testes
- `artist_selftest` (sem janela, kit e `cpu`): .hdr plano e RLE (valores exatos), cortado e falso recusados, 2048×1024
  medido; TIFF/GIF/.hdr/.rudim são texturas; o visualizador aceita .rudim/.tiff; a contagem de vértices e o custo.
- `artist_viewer` (`--artist-test`, janela): um .hdr em `/Content/Q23Test` abre (128×64, 8 mips), a miniatura nasce;
  regravado com 256 de largura, a miniatura e o visualizador acompanham; um .exr abre com o motivo. Área
  `content-browser`.
- `sequencer` ganhou o Mannequin `SeqMan` com o ABP_Mannequin e uma trilha de Animação: o clipe o move (≥ 10 quadros
  sem o Anim BP) e no fim o Anim BP volta.
- Assistente: artigo `texture-viewer` (pt/en), linha `content-browser` e área `auditoria-artista` no `coverage.txt`,
  item 9 no artigo `sequencer`, três buscas no `assistant_test.c`.

### Suíte (18/11)
`tools/suite.ps1 -Visivel -Areas content-browser,sequencer,animacao,assistente,materiais,editor,idiomas` (kit + áreas, 119
testes, 11:54) com dois editores de outras sessões abertos na GPU: 102 passaram, 17 falharam — 15 por fps/tempo
(`FAIL: N fps < 200`, `cable_hang` 52,5 µs > 50, a travada do `world_partition`) e `interaction`/`async` parados por tempo.
`-Falhas` logo depois, com a GPU livre: **16 de 18 passaram**; ficaram `blueprints` ("the Timeline didn't play",
instável já nas notas do Q7 e do Q22: rodado de novo à mão, passa com 278/296 fps) e `demo_fighting` (fps < 180: base
153/145 × Q23 150/149 em pares, o mesmo nos dois). `artist_selftest`, `artist_viewer`, `sequencer` (com o SeqMan),
`asset_viewport`, `tips_audit`, `ui_tips`, `i18n`, `assistant_pack` e `assistant` passaram. Nenhum VUID nem DEVICE_LOST.

## 3. Plano para o 0.44 (itens no formato do plano 0.43)
Numerados **Q23-n** para não colidir com os do Q24 (IA e programação); a sessão principal renumera ao juntar os dois.
Ordem sugerida em lotes de 3 (memória "plano em lotes de 3 subchats").

**Lote A (o que o artista esbarra primeiro)**
- **Q23-1. Static Mesh Editor de verdade** — Details do asset: LODs (quantos, redução %, tamanho de tela, regenerar
  sempre, importar LOD), colisão simples no asset (caixa/esfera/cápsula/k-DOP/convexo automático, complexa como simples;
  o componente herda, o ator ainda sobrescreve), sockets de Static Mesh, slots de material (o import deixa de separar a
  malha por material), material padrão salvo, canal de UV, recalcular normais/tangentes, resolução do lightmap. Formato:
  um `.rmeshset` (JSON ao lado do `.rmesh`, o `.rmesh` mapeado não muda; versão própria). Pronto: teste
  `static_mesh_editor` (gera 4 LODs com tamanhos de tela, colisão convexa usada pela física, socket anexando um ator,
  2 slots desenhando 2 materiais, reabre igual). Medida: abrir < 30 ms, regenerar LODs (ms por 100 k tri), 0 custo no
  quadro com o editor fechado. Área: `content-browser`, `render`. **M (3-4 d)**.
- **Q23-2. Retarget (IK Retargeter)** — cadeias de retarget e raiz no IK Rig (automáticas pelo `bone_like`); asset
  `.rretarget` (origem, destino, mapeamento automático por nome editável, pose de retarget com gizmo de rotação, FK por
  rotação + metas de IK nas pontas, escala da bacia, root motion); prévia lado a lado no viewport do asset; Exportar
  Animações em lote; no import "usar esqueleto existente"; os editores listam animações compatíveis pelos ossos.
  Pronto: teste `retarget` (Mannequin → um esqueleto de proporções diferentes, pés no chão: deslize < 2 cm/m; mãos
  onde a origem põe ± 3 cm; 50 clipes exportados). Medida: retarget em jogo de 100 personagens (ms CPU), exportar 50
  clipes (s). Área: `animacao`, `ikrig`. **G (5 d)**.
- **Q23-3. Montage completo + prévia da trilha de Animação** — Play Montage com Starting Section, Montage Jump to
  Section, Set Next Section, seções encadeadas e em laço; slots do Anim BP (não fixos); a trilha de Animação do
  Sequencer posa o personagem ao arrastar o cursor (para frente e para trás) e ganha início, rate, peso e blend in/out.
  Pronto: teste `montage_sections` (um combo de 3 seções pulando pela tecla), `sequencer` confere a pose no scrub.
  Medida: custo por Montage ativo. Área: `animacao`, `sequencer`, `blueprints`. **M (2 d)** (mexe no enum de nós: juntar
  depois do Q24 se ele também criar nós).

**Lote B**
- **Q23-4. Mesh Paint (Shift+4)** — cor por vértice com pincel (raio, força, queda, canais RGBA, preencher, apagar),
  por instância (override no ator, salvo no nível) ou no asset (`RMESH_COLORS`); pesos do pin do tecido e da amarração do
  soft body pintados na malha; a queda do pincel também na Paisagem. Pronto: teste `mesh_paint` (traço sintético muda a
  cor por vértice esperada, o material Color Attribute mostra, desfazer volta, o tecido prende onde foi pintado).
  Medida: traço em 100 k vértices < 0,5 ms CPU, envio parcial à GPU. Área: `editor`, `materiais`, `tecido`. **M (4 d)**.
- **Q23-5. Material Editor: desfazer, copiar, comentários, Functions** — Ctrl+Z/Y (o mesmo esquema do Sequencer),
  Ctrl+C/V/D de nós, caixas de comentário (C), Material Functions (`.rmatfn`, entradas/saídas, assadas dentro do
  material), Texture Parameter (troca na instância sem assar o grafo inteiro), Static Switch, grupos e ordem dos
  parâmetros, contagem de samplers. Pronto: teste `mat_editor_ops` (copiar/colar/desfazer em lote, uma Function usada
  por 2 materiais muda os dois). Medida: re-bake de uma instância com Texture Parameter (ms). Área: `materiais`. **M (3 d)**.
- **Q23-6. Asset de textura e ajustes** — `.rtexset` ao lado da imagem: sRGB, compressão (BC1/3/4/5/6H/7, nenhuma),
  tamanho máximo, LOD bias, mips (filtro, nitidez), grupo; ajustes (brilho, saturação, matiz, curva, inverter, alfa a
  partir de um canal) aplicados no bake; memória estimada na GPU; OpenEXR (ZIP/PIZ pelo `re_zlib`, metade/float);
  .hdr acima de 8 bits para HDRIs do céu; Reimportar. Pronto: teste `texture_settings` (normal map em BC5 linear, um
  ajuste de saturação muda o bake, EXR de 16 bits lido). Medida: bake com e sem ajustes (ms), memória. Área:
  `materiais`, `content-browser`. **M (3 d)**.

**Lote C**
- **Q23-7. Edição de pose e Control Rig** — (a) gizmo de rotação de osso no viewport do asset e na viewport do nível,
  Animation Mode, chaves de osso numa trilha do Sequencer, assar para Anim Sequence; (b) Control Rig: asset `.rcrig`
  com grafo no editor dos Blueprints compilado para o bytecode da VM (sem alocação), controles com formas, FK/IK com
  troca e espaços (Two Bone, FABRIK do `ik_solver.c`), solve para frente e para trás, trilha de Control Rig no
  Sequencer, nó Control Rig no Anim Graph e em jogo. Pronto: testes `pose_key` (chave de osso tocada e assada igual) e
  `control_rig` (braço IK seguindo um controle, assado para sequência igual ± 0,1°). Medida: 50 personagens com Control
  Rig < 0,5 ms CPU nos workers. Área: `animacao`, `sequencer`, `ikrig`. **G (8-10 d; (a) pode ir antes sozinho)**.
- **Q23-8. Merge Actors, pivô e blockout em malha** — Ator > Juntar Atores (malhas + materiais num Static Mesh, com
  LODs), Converter em Static Mesh (blockout `proto` e formas básicas), pivô editável (Alt+meio arrasta, Set Pivot
  Offset, Bake Pivot), encaixe em superfície e alinhar/distribuir. Pronto: teste `merge_actors` (3 atores → 1 malha,
  mesmos pixels ± 1%, draws caem). Medida: draws e ms antes × depois numa sala de blockout. Área: `editor`,
  `prototipagem`. **M (2-3 d)**.
- **Q23-9. Conveniências do Content Browser** — Reimportar (Static Mesh, textura, material, som), Find References e
  Reference Viewer (grafo de dependências lido dos assets), Duplicate, Migrate (copiar com dependências para outro
  projeto), Size Map, filtro por tipo e favoritos, edição de vários atores no Details (os campos iguais aparecem, os
  diferentes com "—"). Pronto: teste `content_refs` (renomear/mover não quebra referências mostradas; Migrate leva as
  dependências). Medida: índice de referências de 5 000 assets (ms, em worker). Área: `content-browser`, `editor`. **M (3 d)**.

**Lote D (média/baixa)**
- **Q23-10. Modeling Mode (Shift+5)** — primitivas com parâmetros (toro, cápsula, disco, seta, escada/rampa de malha),
  PolyEdit numa malha half-edge (extrudar, inset, bevel, ponte, apagar, soldar), PolyGroups, espelhar, normais; depois
  booleanas (predicados exatos) e desdobrar UV (seams + LSCM, layout). Pronto: teste `modeling_ops` (cada operação
  fechada/manifold, desfazer exato). Medida: operação em 50 k tri < 16 ms. Área: `editor`. **G (8 d; booleanas e UV
  mais 5 d)**.
- **Q23-11. Sequência de animação e esqueleto** — root motion por sequência, aditivo com pose base escolhida, curvas
  genéricas + Get Curve Value, notify states e várias trilhas de notify, Pose Asset, Blend Space como asset (mais de 9
  amostras), régua com números e passo por quadro; esqueleto: adicionar/renomear ossos, ossos virtuais, gizmo de
  rotação e escala do socket; corrigir a Bind Pose editada (reconstruir `bind_world/bind_inv`). Pronto: testes
  `root_motion`, `anim_curves`, `bind_pose_edit`. Área: `animacao`. **M (4 d)**.
- **Q23-12. Niagara: eventos, sub-emissores e escalabilidade** — eventos de morte/colisão com sub-emissores,
  parâmetros de usuário (no ator e por Blueprint), escalabilidade por distância/preset, editor de curvas livre,
  desfazer, partículas suaves. Pronto: teste `particles_events` (faísca que nasce na colisão), `particles_lod`. Medida:
  5 M partículas como hoje + eventos. Área: `particulas`. **M (4 d)**.

## 4. Merge (branch `claude/eager-bassi-73cf1b`, a partir de `merge-lote7` `2669c85`)
- **Arquivos**: `src/mat_image.c` (`mat_hdr_decode`, `mat_hdr_rgba8`, `ext_is`, `#include <math.h>`, o .hdr em
  `mat_load_image_rgba`), `src/asset_viewer.c/.h` (viewer: `mip/mips/hdr/exposure/error/mtime/fsize` no `V`, `hdr_shrink`,
  `asset_mesh_used_verts`, `.rudim` nos `exts`, `thumb_watch` no worker, `Thumb.mtime/fsize`, `asset_viewer_state`,
  `asset_thumb_state`, declarações do teste), `src/content.c` (tif/tiff/gif), `src/anim.c/.h` (`anim_uses_blueprint`),
  `src/sequencer.c/.h` (`restore_anim`, `SeqStats.anim_restored`), `src/anim_editor.c` (Criar Montage, Add Notify, Remover
  Seção), `src/seq_test.c` (SeqMan), **novos** `src/artist_test.c` e `src/ui_tips_table_q23.c`, `src/main.c`
  (`--artist-test`, `--artist-selftest`, a lista `automated`, o gancho do quadro depois do `avp_test`), `src/ui_tips.c`,
  `src/i18n_table.c` (bloco Q23 no fim), `CMakeLists.txt` (`artist_test.c` depois do `asset_viewport_test.c`; os 5
  `ui_tips_table_q23.c`; os testes `artist_selftest` e `artist_viewer` depois do `asset_viewport`), `tools/suite_areas.cmake`
  (`content-browser`, `RE_QUICK`, `RE_CPU`), `tools/tips_audit.py`, `assistant/articles/texture-viewer.md` (novo) e
  `sequencer.md`, `assistant/coverage.txt`, `src/assistant_test.c`, `RealEngine.md` (índice), notas `editor.md`,
  `landscape.md`, `materiais.md`, `particulas.md`, `content-browser.md`, `ikrig.md` e esta.
- **Bits e formatos novos**: nenhum formato de arquivo muda; `SeqStats` ganhou `anim_restored` (no meio da struct: quem
  inicializa por posição, ninguém hoje); nenhum binding, flag ou nó de Blueprint novo.
- **Conflitos prováveis**: as listas que todo subchat toca — `CMakeLists.txt` (5 listas do `ui_tips_table_*` e os
  `add_test`), `src/ui_tips.c`, `tools/tips_audit.py`, `tools/suite_areas.cmake` (RE_QUICK/RE_CPU), fim do
  `src/i18n_table.c`, `src/main.c` (flags e a lista `automated`), `assistant/coverage.txt`, `src/assistant_test.c`,
  `RealEngine.md`. O Q24 (IA e programação) e o Q25 (VR) devem mexer nas mesmas listas: juntar as duas linhas.
