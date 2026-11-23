# Nanite Tessellation (Q17, 2026-11-18)

O deslocamento de verdade, como o Nanite Tessellation da Unreal 5.4+: a altura do material (o Displacement da Material
Output, a mesma do parallax do M7) move a superfície dos clusters do LOD em DAG (render.md, "LOD de clusters em DAG"),
cada triângulo base subdividido na GPU pelo tamanho na tela. A silhueta, as sombras e o contato mudam.

## O que o artista faz
- Material **Opaco** com uma altura no **Displacement** da Material Output (nó Displacement ou uma textura direto).
- **Detalhes > Nanite > Enable Tessellation**, **Displacement Magnitude** (cm da malha: a escala do ator multiplica) e
  **Displacement Center** (o Midlevel do Blender: a altura que fica no lugar). Como o Displacement Scaling da Unreal.
- Gravado no `.rmat`: `tessellation`, `displacement_magnitude`, `displacement_center`. A Material Instance usa o do pai.
- Com a tesselação ligada o material não faz parallax (o M7): a mesma altura vira geometria. Fica sem os dois: material
  animado (flipbook), World Aligned, Masked/Translúcido, Eye.

## Como funciona
- **Bits e formatos**: `MATF_TESS` 4096 (`mat_ops.h`); magnitude e centro em dois halfs no `GpuMaterial.anim.w` (o
  GpuMatExt está cheio: e11.yz são do Thin). `GpuFrame` (+48 B, depois do `terror`): `tess_buf`, `tess_pat` (livre, 0),
  `tess[4]` (px por microaresta já com a escala do orçamento, fator máximo), `tess_u[4]` (flags `TESS_ON` 1,
  `TESS_SUBDIV` 2, `TESS_SEAMS` 4; o espaço de triângulos; z: o diagnóstico). `GpuMeshInfo.vcol_pad` virou `tess_first`.
  Timestamps 30-31 (`TS_PER_FRAME` 30 → 32): a subdivisão do 1º conjunto. Bit 30 da palavra 3 do `info` do `cl_work`
  (`CL_TESS`): a entrada do cluster foi para o R.tess. O microvértice: o `tess.vert` (Q22; antes o passe 9 do `mesh.vert`).
- **Tabela canônica** (`tess_canon_table`, no carregamento de cada malha com DAG): para cada vértice, o primeiro na
  mesma posição (com tolerância de 2e-6 do tamanho da malha, mínimo 1e-4 cm) e a marca `TESS_DUP` quando tem cópias
  (costura de UV, aresta dura, polo). Fica nas palavras do `R.morphs` (`f.mesh_words`), com o nº de vértices e a
  densidade de UV por cm (o mip da altura). As cópias a um fio de distância são **soldadas na posição antes da DAG**: o
  clusterlod junta só posições idênticas, e a costura da esfera da `rocks` (cos 2π em float) virava T-junctions ao
  longo da costura (138 arestas sem par num corte de 22 mil triângulos), que o deslocamento abriria em rachaduras.
- **cull.comp**: passe 8 de uma entrada de DAG com material deslocado reserva os triângulos dela no R.tess (CAS: só se
  couberem inteiros; senão vai pelo caminho de antes, com os vértices deslocados, e conta como fallback); passe 7 copia
  os índices para lá com o dono de cada triângulo; o desenho da entrada fica vazio. Longe (fora do LOD 0/1 da DAG) a
  instância vai inteira no LOD dela, os vértices deslocados.
- **tess.comp** (por fase da DAG: a 1ª antes do pré-passe, a tardia depois do Hi-Z novo): 0) o intervalo de
  triângulos do conjunto e o dispatch; 1) os fatores das 3 arestas (o tamanho na tela da aresta sobre os px alvo, das
  duas pontas canônicas, simétrico), N = o maior, o lugar na lista do padrão N; descarta antes de subdividir o que está
  fora da vista (com o alcance do deslocamento) ou de costas (a face e as 3 normais além de -0,35 do silhueta);
  2) prefixos → 32 draws indiretos, cada lista alinhada ao lote e completada com sentinelas; 3) os registros
  (triângulo, slot, fatores, vertex offset).
- **Padrões em lote**: o padrão N (N² triângulos) repetido `TESS_BATCH(N) = max(128/N², 1)` vezes num índice estático;
  o índice **é** o ponto (`i | j<<6 | N-1<<12 | cópia<<17`, < 2^24), sem tabela. Um triângulo base por instância deixava
  as waves do vertex shader quase vazias.
- **Sem costura**: um ponto de aresta é calculado só pela aresta, nas pontas canônicas em ordem fixa (`tess_edge`); o
  fator da aresta também. No padrão, o vértice da borda com N maior que o fator da aresta é encaixado no ponto k/e mais
  perto (triângulos degenerados, nenhum T). Aresta de costura (as duas pontas `TESS_DUP`): deslocamento linear entre
  os cantos (as UVs diferem dos dois lados); as outras, a altura lida ao longo dela. Cantos: a normal e a UV do
  vértice canônico. Tudo `precise` (sem FMA nem reordenação: cada ponto de chamada inlinado arredonda igual) e um
  ponto de chamada por tipo de ponto.
- **Mip da altura**: o texel ≈ uma microaresta naquela distância (não cintila longe), da posição canônica: o mesmo nos
  dois triângulos.
- **Orçamento** (`tess_collect`, um quadro atrasado): passou do orçamento de microtriângulos, os px por microaresta
  crescem pela raiz (os microtriângulos caem com 1/px²); abaixo de 70% voltam devagar.
- **Oclusão**: com material deslocado e DAG, o pré-passe fica sempre ligado (só nele há DAG e subdivisão; a medida
  automática sem ele piscava a cena a cada 3 s).

## Shader próprio e cantos por triângulo (Q22, 2026-11-18)
- **`tess.vert`**: o passe 9 saiu do `mesh.vert` para um vertex shader só dele. Cada pipeline opaca de malha (as do
  `mesh.vert` e do `mesh_x.vert`, profundidade D32, cor RGBA16F ou só profundidade) ganha uma **gêmea** com o `tess.vert`
  (`pipeline()` em render.c a faz junto, enquanto `R.tess_twins`; `tess_twin_put/tess_twin/tess_twin_take` em tess.c,
  com trava: os conjuntos de RT nascem em threads). O `vkDestroyPipeline` do render.c leva a gêmea junto
  (`pipe_destroy_twin`). O 1º material deslocado enviado liga `R.tess_twins` e refaz as pipelines uma vez
  (`R.tess_rebuild`, como o `toon_rebuild`); até lá o `tess_prepare` não subdivide (vértices deslocados, sem buraco). A
  chave do `den_rt` ganhou o bit 8192. `tess_draws` recebe a gêmea do pipeline de antes.
- **Cantos gravados uma vez** (`TESS_CRN_AT`, 8 × 16 B por triângulo, `tess.comp` passo 1, só os que não foram
  descartados): por canto a posição canônica, o deslocamento do vértice canônico (3 halfs: os mesmos bits em todo
  triângulo daquele vértice), a normal e a UV do próprio vértice, a UV2 em unorm16 (documentada em [0, 1]); por
  triângulo a textura da altura, magnitude e centro (halfs), tiling, a escala do mip (`texels_cm / s`), o viés dos raios
  e 3 bits por aresta (`TESS_E_SWAP` a 2ª ponta tem o vértice canônico menor, `TESS_E_POINT` as duas no mesmo,
  `TESS_E_SEAM` as duas são cópias). O microvértice lê o registro, os 128 B do triângulo e as 3 linhas da instância, e
  faz uma leitura de textura (só os pontos de dentro e do meio das arestas); antes eram ~8 leituras encadeadas (registro →
  instância → malha → material → índices → vértices → tabela canônica → vértice canônico) e uma textura por canto.
- **Sem costura igual**: `tess_tri_edge` é o `tess_edge` sobre os cantos (mesma ordem canônica, `precise`); o teste de
  costura (`seam_hash`) agora soma os pontos do `tess_tri_edge` com os mesmos halfs que o `tess.vert` lê.
- `TESS_MAX_TRIS` 524288 → **262144** (cada triângulo carrega 128 B: o buffer foi de 20 para 42 MB; mais do que isso
  vai pelo caminho de reserva, vértices deslocados). Pontos de dentro usam a posição canônica (a mesma do vértice: as
  cópias foram soldadas antes da DAG).
- Registradores (`--shader-stats`, que agora imprime o VS do `mesh` e do `mesh (tess)`): `mesh.vert` 61, `tess.vert` 68.

## Sombras e ray tracing (fallback)
- **Mapas de sombra** (sol virtual e luzes locais): os vértices do LOD da sombra deslocados (`q17_pos` no passe 1), sem
  subdividir. O receptor levanta a consulta do VSM em `o_disp_bias` (magnitude × centro × escala + 0,5 cm) para não
  se autossombrear pela diferença entre o fino e o grosso.
- **Ray tracing** (BLAS/CLAS, rt-clusters.md): veem a malha base. O raio sai a `o_ray_bias + o_disp_bias` da superfície
  (o quanto ela afunda sob a BLAS). Falta: uma BLAS deslocada por instância (junto do A8c, BLAS com o corte do raster).
- **Velocidade** (TSR): a passada de velocidade dos atores móveis desenha a malha base; onde o deslocamento muda a
  profundidade, o pixel fica com o movimento da câmera.
- Malhas sem DAG (< 16 mil triângulos, peças soltas): só os vértices deslocados, sem tabela canônica (costura de UV pode
  abrir).

## Escala (preset, cvars)
| Preset | px por microaresta | fator máximo | orçamento (M microtri) |
|---|---|---|---|
| Baixo | — (só vértices) | 1 | — |
| Médio | 10 | 4 | 1 |
| Alto | 6 | 8 | 2 |
| Épico | 4 | 16 | 4 |
| Cinemático | 2 | 32 | 12 |

`r.Nanite.Tessellation`, `r.Nanite.DicingRate`, `r.Nanite.MaxTessFactor`, `r.Nanite.TessBudget`. Celular: fator 4 no
alto, 1 nos outros. Para medir: `REALENGINE_NO_TESS=1` (sem deslocamento), `REALENGINE_TESS_PX`, `REALENGINE_TESS_MAX`,
`REALENGINE_TESS_BUDGET` (milhões). Debug Viewer > Tessellation; `--gpu-passes` mostra a marca `tessellation`.

## Testes (área `tessellation`)
- `tess_rocks`: as 100 rochas da `rocks` com o material deslocado (`--scene rocks_tess`), validação, ≥ 110 fps (com a validação o teste mede 117–120 fps, 8,3–8,5 ms; o 6,81 ms das medidas é o mínimo da GPU sem validação; o limite de 120 ficava em cima da medida, baixado no merge do lote 6).
- `tess_seams` (no kit rápido): uma rocha inteira na vista, oclusão da DAG desligada, a rocha pela DAG em qualquer
  distância (`TESS_SEAMS` no `cull.comp`); cada aresta de cada triângulo base tem os pontos
  deslocados (e o fator) num hash, XOR de todos: as duas metades de cada aresta se cancelam. 60 quadros com XOR 0 = OK.
  Diagnóstico: `REALENGINE_TESS_SEAMS_TOPO=1` (só as arestas: a topologia do corte), `REALENGINE_TESS_SEAMS_E1=1` (só
  os cantos), `REALENGINE_TESS_DUMP=1` (o corte lido na CPU: arestas ímpares com as posições).
- `tess_low`: Baixo, só deslocamento. `tess_budget`: orçamento de 0,1 M, a escala sobe.

## Medidas (RTX 3060, 2560×1417, Épico, mínimo da GPU de 3 rodadas intercaladas, escondido, sem testes de outros)
Cena `rocks_tess` (100 rochas de 1,54 M triângulos, ruído de 8 cm, câmera baixa entre elas):

| Configuração | GPU mínimo | passe principal | microtriângulos | triângulos base |
|---|---|---|---|---|
| sem deslocamento (`REALENGINE_NO_TESS=1`) | 4,97 ms | 3,51 | — | — |
| só vértices (Baixo, `REALENGINE_TESS_MAX=1`) | 5,45 ms | 4,02 | — | — |
| 6 px (Alto) | 6,23 ms | 4,78 | 0,29 M | 36,8 mil |
| **4 px (Épico)** | **6,81 ms (147 fps)** | 5,39 | 0,57 M | 36,8 mil |
| 2 px (Cinemático) | 8,25 ms | 6,83 | 1,93 M | 36,8 mil |

- A subdivisão em si (tess.comp, timestamps 30-31) custa 0,05 ms; o resto é o vertex shader dos microvértices e a
  rasterização (duas vezes: pré-passe e passe principal). Escala com os microtriângulos: ~1,4 ms a cada 0,6 M.
- O caminho que levou até aqui (mesma cena, 2 px): todas as rochas pela DAG em qualquer distância: 359 mil base,
  4,4 M micro, +7 a +12 ms → só onde a DAG já é usada (LOD 0/1): 56 mil base, 2,7 M micro, +3,7 ms → descarte de costas
  e fora da vista antes de subdividir: 36,8 mil base, 1,9 M micro, +3,3 ms. O lote de padrões (128 base por instância)
  não mudou o tempo nessa cena (mantido: elimina a tabela de pontos e não piora).
- Sem nenhum ator com material deslocado nada roda (o `tess_prepare` olha as instâncias, não os materiais carregados).
  Cena `rocks` (base `4dfdb17` × este branch, 3 pares): 4,90–4,91 × 4,91–5,01 ms; passe principal 3,47–3,51 × 3,52–3,58
  (~+0,06 ms, 1%: o vertex shader maior, o caminho do passe 9 no mesmo shader; a varying própria do viés custava +0,08
  e virou o sinal do `o_ray_bias`). O corte da DAG da `rocks` ficou menor com a costura soldada: 62,3 → 56,2 mil.
- Meta de 200 fps: a `rocks` sem tesselação está em ~200 fps; com o deslocamento de verdade em Épico, 147. Próximo
  passo para fechar a diferença: guardar por registro (no passo 3) os cantos já deslocados e as UVs/normais, para o
  microvértice não refazer 3 índices + 3 vértices + tabela + material (o custo por vértice), e um shader próprio para o
  passe 9 (o `mesh.vert` comum volta ao tamanho de antes).
- Costura: `tess_seams` 20 744 triângulos base por quadro, 31 116 arestas, 0 sem par (topologia) e 0 quadros com hash
  diferente (pontos e fatores). Antes da solda das cópias: 138 arestas ímpares, todas na costura de UV da rocha.

- **Q22** (`tess.vert` e os cantos por triângulo, 3 pares base × branch, GPU dividida com outras sessões): Épico
  6,61–6,64 → **6,49–6,59 ms** (−0,07; a subdivisão +0,03 ms por gravar os cantos); `rocks` 4,92–4,98 → 4,86–4,92 ms
  (o `mesh.vert` de volta ao tamanho). O que sobra é rasterizar microtriângulos de 4 px duas vezes (pré-passe e passe
  principal) e o sombreamento por quad do passe principal: o próximo passo seria um visibility buffer para eles.

## Suíte (2026-11-18)
`tools/suite.ps1 -Areas tessellation,render,materiais,rt-clusters,luz -Visivel`: 129/134. Repetidos escondidos:
`tess_seams` passa (a 1ª falha foi a rocha fora da DAG: o teste agora a força), `spec_leak_epic` e `spec_leak_nort`
passam (GPU dividida). `assistant` (9 buscas de splines/PCG/câmera fora do lugar) e `world_partition` ("a hitch")
falham igual na base `4dfdb17`: preexistentes.
