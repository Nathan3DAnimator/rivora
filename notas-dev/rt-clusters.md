# Ray tracing por clusters (A8, o "Mega Geometry" da NVIDIA)

Desde 2026-11-09 as BLAS das malhas estáticas são feitas de **clusters** onde a GPU tem `VK_NV_cluster_acceleration_structure`
(RTX; a 3060 com o driver 617.42 tem a revisão 4). Código em `src/rt_clusters.c` (isolado: quando sair a versão KHR/EXT
multivendedor, a troca fica nele). Sem a extensão (AMD, Intel, Android, drivers antigos) ou com `REALENGINE_NO_CLAS=1`,
as BLAS clássicas de sempre.

## Como funciona
- **Detecção** (`vkctx.c`): a extensão e a feature `clusterAccelerationStructure` opcionais; as propriedades (triângulos e
  vértices por cluster, índice de geometria máximo, alinhamentos) em `VkCtx`. O log diz "(clusters: ...)" ao lado do RT.
- **Os clusters na CPU** (`rtc_prepare`): `meshopt_buildMeshletsSpatial` (o construtor do meshoptimizer 1.3 feito para ray
  tracing: corta pela heurística de área de superfície, como uma BVH), 64 a 128 triângulos e até 256 vértices; cada
  cluster com os seus vértices (posições copiadas) e índices de 8 bits. Num recarregamento das malhas isso roda no worker
  que já lê os arquivos (async.md); no início, na carga.
- **O acerto acha o triângulo pelo índice de geometria**: cada triângulo leva o seu (o lugar na malha + 1, por triângulo,
  `geometryIndexAndFlagsBuffer`); o `rt.glsl` usa `geometria − 1`, ou o índice da primitiva numa BLAS clássica (geometria
  0). O shader é o mesmo nos dois caminhos e não precisa de capacidade da NVIDIA (`GL_NV_cluster_acceleration_structure`
  ficou de fora). O construtor reordena os triângulos: o original é achado por uma tabela hash dos 3 vértices.
- **Tamanhos exatos** (`rtc_commit`): o driver mede cada CLAS (`OP_MODE_COMPUTE_SIZES`), cada uma vai para o seu lugar num
  buffer do tamanho certo (`EXPLICIT_DESTINATIONS`); depois o mesmo para as BLAS, que referenciam as CLAS da malha. Nada
  alocado para o pior caso. Três submissões esperadas (só na carga).
- O que fica clássico: as BLAS dinâmicas (tecido, skins, multidões: `gi_dyn_blas_*`) e as refeitas com opacity micromap
  (`gi_blas_omm`, opt-in).

## Medidas (RTX 3060, 1440p, Épico)
| cena | BLAS clássicas | clusters | fps clássico → clusters |
|---|---|---|---|
| house (96 malhas, 0,32 M tri) | 21,6 MB | 6,8 MB (−69%) | 254 → 254–256 |
| heavy (7 malhas, 2,1 M tri) | 142,9 MB | 58,6 MB (−59%) | 59–60 → 60–61 |
| RPG Oakvale (43 malhas, 0,37 M tri) | 24,9 MB | 14,1 MB (−43%) | 138 → 148 |
- Imagem igual (house: diferença média 0,006/255), cintilação igual (0,073% house, 0,019% RPG), validação limpa.
- **A ordem importa:** clusters na ordem do buffer de índices deixaram o traçado 2× mais lento na heavy (28 fps: caixas
  grandes e sobrepostas); pela curva de Morton, 46 fps; pelo construtor espacial do meshoptimizer, igual ao clássico.
- Custo de construção: heavy 1,1 s de CPU (meshoptimizer) + 44 ms de GPU; RPG 109 + 17 ms.

## Falta (A8)
- (Q17) Nanite Tessellation: os raios veem a malha base; o raio de uma superfície deslocada sai afastado o quanto ela afunda
  (tessellation.md). A BLAS deslocada entra junto do c).
- c) BLAS por instância com o corte de LOD do raster (hoje o raio vê o LOD 0 de cada malha), com os builds indiretos
  saindo do `cull.comp`: acaba a autossombra na troca de LOD.
- d) TLAS particionada (`VK_NV_partitioned_acceleration_structure`): só as partições que mudaram.
- e) Templates de cluster para o que se deforma (tecido, skins, oceano, multidões).
- f) OMM junto com as CLAS; h) a variante portátil (BLAS por grupo de clusters em KHR para AMD/Intel).
- Paralelizar o `meshopt_buildMeshletsSpatial` por malha no job system (1,1 s na heavy).
- Testes `rt_clusters` (validação, exige a linha "BLAS de clusters (A8)": precisa de uma RTX com a extensão) e
  `rt_classic` (o mesmo com `REALENGINE_NO_CLAS=1`).
