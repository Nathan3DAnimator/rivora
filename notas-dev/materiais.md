# Materiais (`material.c`, `mat_nodes.c`, `mat_editor.c`, `shaders/material_bake.comp`, `mat_vm.glsl`)

Pedido: sistema de materiais com nodes semelhantes aos do Tlender e **compatíveis** com eles (retrocompatibilidade), muito
otimizado: um material muito complexo não pode derrubar o FPS. Material Instances (como a Unreal) para expor
propriedades; Blueprints alteram as propriedades de um material de um ator.

## Desenho
- **Nodes** = os do Tlender/Blender (`mat_nodes.c`, cópia adaptada do `tlender/src/nodes.c`): mesmos `idname`, nomes e
  ordem dos sockets, padrões e enums (`shaders/tl/kernel_svm_types.h` é o do Tlender, copiado). O `.rmat` (JSON) guarda
  nodes por `idname` e ligações por **nome de socket**: um grafo do Tlender abre igual. 39 nodes: Principled BSDF,
  Diffuse, Glossy, Transparent, Emission, Mix/Add Shader, Image/Noise/Voronoi/Checker/Gradient/Wave/White Noise Texture,
  Mix (19 modos), Invert, Hue/Sat, Bright/Contrast, Gamma, Mapping, Bump, Normal Map, Color Ramp, Math (41 ops), Vector
  Math (27), Map Range, Clamp, Separate/Combine XYZ/Color, RGB to BW, Texture Coordinate, UV Map, RGB, Value, Fresnel,
  Layer Weight, Material Output.
- **Matemática dos nodes = a do Tlender**: `shaders/tl/kernel_compat.h` e `kernel_svm_tex.h` copiados (dialeto C/GLSL do
  Tlender: ruído de Perlin/fBm/multifractais, Voronoi, xadrez, ondas, modos de mistura, `svm_math`,
  `svm_vector_math`, `svm_mapping`...). O resultado bate com o Tlender/Blender.
- **Compilador** (`material_gpu.inc`): grafo → bytecode por registradores (`mat_ops.h`: um op por node, constantes,
  conversões implícitas de socket, closures como 3 registradores), alocação por tempo de vida.
- **Tudo é assado** na GPU (`material_bake.comp` roda o bytecode por texel do UV na resolução do material, 64–4096):
  cor base (sRGB, alfa), normal tangente + rugosidade + metálico (RGBA8), emissivo (RGBA16F, só se houver), mips por
  blit, a cor média (o último mip) para a GI. **No jogo um pixel lê 2–3 texturas, qualquer que seja o grafo** (um
  material de 48 instruções custa o mesmo que um de 9). O frame bindless é o set 1 do pipeline (`u_mat_tex[]`).
- Tentativa descartada: rodar a parte dinâmica por pixel num interpretador dentro do `mesh.frag` (+0,6 ms no passe
  principal com 1 milhão de objetos, mesmo sem material: pressão de registradores). Em vez disso **cada variante tem a
  própria textura assada**.
- **Parâmetros**: um node RGB / Value com "Parameter Name". **Material Instance** (`.rmi`: pai + valores) → texturas
  próprias assadas com seus valores (se o pai tem parâmetros; senão compartilha as do pai). **Instância dinâmica por
  ator**: Blueprint **Set Scalar / Vector Parameter Value on Materials** (como a Unreal) cria a instância do ator e
  re-assa só ela antes do próximo quadro (uma vez por quadro, quantos valores mudarem). PIE ao sair as descarta.
- Visão (Fresnel, Layer Weight) assada de frente (limitação conhecida; o caminho por pixel volta quando houver
  permutações de pipeline por material).
- Instância: o slot do material vai em `emissive_rough.x` (bits) com `INST_MATERIAL` (a instância continua 112 B: com 1
  milhão de instâncias cada byte conta), `.y` a repetição do UV; o UV e o UV2 num `vec4` só do `mesh.vert`.

## Editor
- Content Browser > Adicionar: **Material**; a **Material Instance** nasce do clique direito num material (Criar > Material
  Instance, Q6). Duplo clique abre o editor.
- Canvas como o do Blender (cores de cabeçalho por categoria, sockets por tipo, curvas), Shift+A / clique direito: menu
  por categoria, arrastar de saída para entrada liga, arrastar uma entrada solta, Del/X apaga, roda dá zoom, botão do meio
  ou direito arrasta. Details: propriedades, entradas, rampa (paradas), imagem (campo, arrastar do Content Browser ou
  "..."), nome do parâmetro; sem seleção: resolução e **World Aligned Grid**. Prévia das texturas assadas.
- Details do ator > Materials: **Material** (lista ou arrastar), **UV Tiling**, Editar Material.
- `Content/Materials/M_Grid.rmat` (a grade da Unreal: quadrados de 1 m, linhas de 10 cm, alinhada ao mundo) e
  `Materials/Examples/` (M_Marble, M_TintedChecker com parâmetro Tint, M_RustedMetal, M_EmissivePanel) criados na
  primeira execução.

## Medidas (RTX 3060, 2560×1417)
| | |
|---|---|
| Bake 1024² (9 / 20 / 33 / 48 instruções) | 4,1 / 3,6 / 5,2 / 10,5 ms (só na edição) |
| Cena `--scene materials` (5 materiais, 1 instância dinâmica) | 549 fps |
| 1 M objetos (`scale_rt`), antes / depois de toda a sessão | ~165 / ~157 fps (sem material nos objetos; +0,3 ms a investigar) |
Teste `materials` (validação, > 350 fps).

## Falta
- Permutações de pipeline para materiais dependentes da visão; Texture Parameter; as curvas (RGB/Vector/Float Curves) e o
  resto dos nodes do Tlender. (BC7 e o Masked já existem; a lista do que falta contra a Unreal: auditoria-artista.md, Q23.)

## Blend Mode Masked e Two Sided (2026-11-01)
- Details do material (sem seleção) > Material: **Blend Mode** (Opaque / Masked), **Opacity Mask Clip Value** (0,3333,
  o padrão da Unreal) e **Two Sided** — como na Unreal. O alfa é o do Principled BSDF, assado no canal A da cor base.
  `.rmat`: `blend_mode: "Masked"`, `opacity_mask_clip`, `two_sided` (ausentes: Opaque, um lado).
- GPU: `MATF_MASKED` (+ limite nos bits 24-31) e `MATF_TWO_SIDED`. O renderer desenha as malhas desses materiais em
  "faixas masked" (render.md); as opacas não pagam nada.
- Imagens `.tga`: decodificadas em `mat_image.c` (o WIC do Windows não tem codec de Targa; cópia do leitor do Tlender).

## Atalhos da Unreal no Material Editor (2026-11-02)
Tecla + clique no canvas: 1 Value, 3 RGB, T Image Texture, M Math (Multiply), A Math (Add), L Mix, U Texture Coordinate.
Com um fio arrastado, a tecla cria o node no mouse ligado na 1ª entrada. Fio solto no corpo do node: 1ª entrada livre do
mesmo tipo.

## Streaming de texturas (sugestão #51, 2026-11-04)
As texturas assadas de cada material e Material Instance ficam no tamanho que a tela pede, dentro de um orçamento
(`src/material_stream.inc`). Ideia da Unreal (mip por tamanho na tela, pool com orçamento), adaptada ao bake na GPU.
- **Necessidade**: `render.c` `stream_sweep` percorre 16 384 static meshes por quadro, a tela também e as de fora dela
  (girar a câmera não espera textura), e anota o tamanho de cada material em pixels: o diâmetro dos bounds no ponto
  mais perto, vezes o tiling (`uv_scale`). Uma passada pelo mundo fecha no máximo 10× por segundo. O alvo de cada
  variante é a potência de 2 acima disso, entre 256 e a resolução do material. Quando os alvos não cabem em 90% do
  orçamento, os materiais fora de vista vão primeiro ao mínimo e depois todos caem pela metade juntos.
- **Orçamento**: 35% da maior heap device-local; `REALENGINE_TEX_BUDGET_MB` sobrepõe (o teste usa 1536).
  `REALENGINE_TEX_STREAMING=0` desliga. Um bake (edição, carga) usa a resolução cheia enquanto ela cabe na metade do
  orçamento, então no projeto comum nada muda. Sem espaço, começa em 512 (ou 256) e sobe quando chega perto.
- **Transições**: no máximo uma por quadro, sempre no command buffer do próprio quadro, sem esperar a GPU.
  - Subida: em degraus de 4× (512 → 2048 → 4096), assando em faixas de linhas (`BakePush.org`). Cada faixa tem o
    tamanho medido por timestamps para custar 0,4 ms de GPU (num material de 48 instruções com bump: ~58 mil texels).
  - Descida: `vkCmdCopyImage` dos mips menores, sem bake, só sob pressão de memória (85% do orçamento) e depois de 2 s
    com necessidade menor. Com memória sobrando, o que já está residente fica.
  - Troca: só os 3 índices de textura do slot são reescritos, no slot da variante e nos que dividem as texturas dela.
    Isso acontece quando o quadro que assou já terminou. As texturas antigas são liberadas
    `RE_FRAMES_IN_FLIGHT + 1` quadros depois.
- **Memória**: as texturas saem de blocos de 256 MB com first-fit, e as faixas livres se juntam ao liberar. Antes eram
  um `vkAllocateMemory` cada, que custava 0,3–1,5 ms no driver e esbarrava no limite de ~4096 alocações. Com os blocos,
  os picos de CPU caíram de 529 para 6 numa corrida de 10 s. O set bindless (agora com 4096 texturas) usa update after
  bind e update unused while pending; sem esses recursos, o streaming fica desligado.
- **Fora do streaming**: materiais animados (flipbook), instâncias dinâmicas dos atores e materiais da multidão
  (`mat_stream_pin`: as partes não passam pela varredura).
- **Debug Viewer** (F10), lista "Texture Streaming": MB usados/orçamento, subidas/descidas, texels assados no quadro, e
  por material o tamanho atual → alvo / cheio e os MB.
- **Teste** `texture_streaming`: `--stress-textures 400`, 400 cubos com Material Instance 4096 própria (72 GB se
  tudo ficasse cheio), orçamento de 1,5 GB e câmera voando a 15 m/s (`--stress-fly`, 0 deixa parada). Cobra o
  orçamento respeitado, pelo menos uma subida, pelo menos uma descida e > 250 fps.

| Medida (RTX 3060, 2560×1417, 400 cubos 4096) | |
|---|---|
| Câmera parada / voando | 629 / 618 fps (pior quadro 4,7 / 7,8 ms) |
| Residente | 1475 de 1536 MB (802 texturas) |
| CPU do streaming (por quadro / pior) | ~0,02 / 0,8 ms (pior: o 1º bloco de 256 MB) |
| GPU de uma faixa de bake | 0,4 ms (adaptativo; a 1ª faixa: ≤ 1,4 ms) |
| Antes dos blocos e da faixa adaptativa | 458 fps, pior quadro 9,4 ms |

## World Aligned (2026-11-04)
O WorldAlignedTexture da Unreal como opção do material (Material Editor: "Alinhado ao Mundo" e "Tamanho do Ladrilho").
O `mesh.frag` troca a UV da malha pela posição no mundo projetada no eixo dominante da face, dividida pelo tamanho do
ladrilho em cm (`MATF_WORLD_UV`; o tamanho vai no `uv_scale` do slot). A base tangente sai das derivadas dessa UV, por
isso o normal map fica certo nas seis direções. Paredes e pisos de qualquer escala (protótipos, blocos das demos) repetem
a textura num só tamanho, sem esticar. Custo: um ramo uniforme por material. As instâncias herdam do pai.

## Translucent e cobertura nos mips (2026-11-06)
- **Blend Mode Translucent:** o Alpha (do Principled BSDF) é a opacidade. Desenhado num passe próprio, sem ordenar,
  sem sombra (render.md, "Translúcidos").
- **Masked:** depois do bake, os mips da cor base recebem a escala de alfa que preserva a cobertura do recorte do mip 0
  (`alpha_hist.comp`, render.md "fase 3").

## BC7: texturas comprimidas na GPU (2026-11-06, fase 5 das malhas pesadas)
As texturas assadas (cor base e normal/rugosidade/metal) e as imagens dos nós Image Texture eram RGBA8: 4 bytes por
texel, 85 MB por textura 4096 com mips. Agora vão para **BC7**: 1 byte por texel, um quarto da memória e da banda das
leituras.
- **`bc7.comp`:** uma thread por bloco 4x4, no modo 6 (RGBA juntos, extremos de 7 bits com p-bit, índices de 4 bits).
  - Os extremos saem do eixo principal do bloco (iteração de potência na covariância).
  - Um ajuste por mínimos quadrados aos índices, mantido quando o erro cai.
  - O p-bit de cada extremo é escolhido pelo erro.
- **`tex_to_bc7`:** os blocos de cada mip vão para um buffer e são copiados para uma textura BC7 nova, só de leitura
  (sem view de storage).
  - **Bake:** comprime no mesmo envio do bake. Um Masked comprime depois da cobertura dos mips, que lê o histograma na
    CPU. As RGBA8 são enterradas e os slots que compartilham as texturas recebem os índices novos (`st_share`).
  - **Imagens de origem:** comprimidas ao carregar. O bake lê a versão comprimida.
  - **Streaming:**
    - Subida: assa em RGBA8 em ladrilhos, comprime no fim, no mesmo quadro, e o buffer é enterrado como as texturas.
    - Descida: copia os mips BC7 como estão.
    - O orçamento conta 2 bytes por texel e, na subida, o pico RGBA8 mais BC7 (`st_bytes_peak`). Sem esse pico, uma
      subida a 4096 passava 100 MB do orçamento.
- **Bake mais curto:** 400 materiais 4096 levavam 6,8 s com um envio por compressão; com a compressão no envio do
  bake, 3,4 s (sem BC7: 1,8–2,8 s).
- `REALENGINE_NO_BC7=1` mantém RGBA8, para comparar.
- **Medido** (RTX 3060, 1440p, Épico):

| Cena | Memória das texturas | Mínimo da GPU | PSNR da imagem final |
|---|---|---|---|
| Vila RPG (texturas Poly Haven) | 1211 → 343 MB | 7,32 → 7,15 ms | 40,8 dB |
| `heavy` (rocha e cards 4096) | 491 → 163 MB | 5,53 → 5,47 ms | 52,6 dB |
| `texture_streaming` (400 materiais 4096, 1536 MB de orçamento) | todos a 4096 cabem: 1423 MB, nenhuma descida | — | — |

  O teste `texture_streaming` passou a usar 512 MB de orçamento, para continuar havendo pressão de memória: 39 subidas,
  333 descidas, 466 de 512 MB, validação sem erro.
- **Fica de fora:** a emissão (RGBA16F) continua sem compressão (seria BC6H).

## Pinos sem efeito (2026-11-08, plano de melhorias M1a)
O Principled BSDF tem os 30 pinos do Blender, mas o compilador (`material_gpu.inc`) lê só Base Color, Metallic,
Roughness, Alpha, Normal, Emission Color e Strength (0, 1, 2, 4, 5, 26, 27). Coat, Transmission, Subsurface,
Anisotropic, Specular Tint, Sheen, Thin Film, IOR, Diffuse Roughness e Specular IOR Level não mudam nada ainda.
- `mat_input_has_effect(type, sock)` (`mat_nodes.c`) é a tabela; `mat_node_unused_inputs` diz quais estão definidos
  (valor diferente do padrão ou ligados). **Mude os dois juntos com o compilador.**
- Editor: o pino sem efeito fica cinza com "(ainda sem efeito)" e uma dica; o painel avisa em laranja quais estão
  definidos sem efeito ("o valor fica guardado"); o nó ganha um "!" laranja no título.
- Import glTF: transmissão, clearcoat, sheen, specular, anisotropia, iridescência e volume avisam no Registro de Saída.
- **Bug achado**: o Glossy BSDF lia as entradas deslocadas em um (a cor do pino Roughness, a rugosidade do Anisotropy, a
  normal do Tangent). Agora Color 0, Roughness 1, Normal 4. Só materiais importados do Tlender/Blender usam Glossy.
- Teste `mat_pins` (`--mat-pins-test`): um Value ligado em cada uma das 36 entradas do Principled e do Glossy é compilado
  exatamente quando a tabela diz "com efeito"; Coat Weight definido aparece no aviso, os padrões não.
- Artigo do Assistente `material-pins-no-effect`. Os lobos vieram no M1b (abaixo).

## Os lóbulos do Principled (2026-11-09, plano de melhorias M1b)
Coat, Anisotropic, Subsurface, Specular Tint / IOR Level, IOR e Transmission passam a ter efeito **como constantes do
material**: o valor do pino (sem ligação) do Principled que alimenta o Surface da Material Output (direto ou por reroute;
um Mix / Add Shader no caminho: sem lóbulos). `mat_input_const_only` diz quais são; ligados, continuam "sem efeito" (o
editor acinzenta só quando ligados; `mat_node_unused_inputs` só os reporta ligados).
- **Por quê constantes:** o material é assado em 2-3 texturas (cor, normal + rugosidade + metálico, emissão). Assar mais
  canais custaria memória e leituras em todo pixel; quase todo uso real (pintura de carro, vidro, pele, metal escovado) é
  uniforme por material. Um canal assado a mais fica para quando houver demanda.
- **Onde:** `principled_ext` (`material_gpu.inc`) → `Compiled.ext` → uma tabela `GpuMatExt` (64 B por slot) depois das
  GpuMaterials no mesmo buffer (`mat_common.glsl` `GpuMatExtBuf`). A GpuMaterial ficou em 64 B: com 128 o shader a carregava
  inteira e o `mesh_rt` fazia mais spill. Flag `MATF_EXT`.
- **No `mesh.frag`, só nas variantes `RE_EXT`** (`mesh_x`, `mesh_rt_x`, `mesh_rt_t_x` e as `_a`): o render as cria no
  primeiro quadro em que um material com lóbulos é enviado (`mat_ext`, `R.any_ext`; log "the RE_EXT pipelines") e as usa
  desde então. As variantes de sempre ficam exatamente como antes (o `mesh_rt` estava no teto de 128 registradores: cada
  acréscimo, até um registro global, custava um degrau de 16 B de spill). Com Baixo/sem RT e mapas de luz local no quadro,
  a `mesh_lsm` tem prioridade (sem os lóbulos naquele quadro). Criadas na abertura, as variantes atrasavam o início e o
  `house_walk` media outro trecho do caminho (cintilação 0,110 → 0,137%): por isso só sob demanda.
  - `g_ext`: o slot em 12 bits e a tangente da anisotropia octaédrica em 2 × 10; `g_f0d`. O `brdf` lê a tabela:
  - **Coat:** GGX próprio na normal interpolada (o normal map fica por baixo), F0 pelo IOR do coat; a base atenuada por
    (1 − peso × Fresnel) e tingida pelo Coat Tint. Na indireta, o reflexo do coat é o do traço quando a base é lisa, senão
    o céu nítido na normal do coat, misturado à irradiância pela rugosidade do coat.
  - **Anisotropic:** GGX anisotrópico (Burley 2012, aspecto √(1 − 0,9·a)) na tangente do UV girada pela rotação.
  - **Subsurface:** a luz passa do terminador (wrap) por canal: raio normalizado × escala (5 cm do Blender: meio wrap no
    canal maior) × peso.
  - **F0 dos dielétricos:** ((IOR − 1)/(IOR + 1))² × 2 × Specular IOR Level × Specular Tint (o 0,04 do padrão). Um F0 a
    menos de 0,01 do padrão (o IOR 1,45 antigo do Blender) não liga as variantes sozinho.
  - **Transmission:** o material vira translúcido (`mat_translucent`, passe OIT); opacidade × (1 − transmissão × (1 −
    metálico)), também nas variantes translúcidas de sempre (barato, fora do teste de spill).
- **Bug achado e corrigido:** um material translúcido numa cena sem luz local que projeta sombra lia os históricos do
  MegaLights (`ml_hist`) em UNDEFINED (o split nunca rodava): o passe OIT agora os põe em GENERAL antes.
- **Sheen (M3) e Thin Film (M4), 2026-11-09:** na mesma tabela (e4: peso e rugosidade do sheen, espessura em nm e IOR
  da película; e5: a tinta do sheen; a GpuMatExt passou a 96 B). Sheen: o lóbulo Charlie (Estevez e Kulla 2017) com a
  visibilidade de Ashikhmin, somado à luz direta: a borda clara do veludo. Película: a fase do caminho óptico por
  comprimento de onda (650 / 510 / 475 nm) colorindo o Fresnel, na luz direta e no reflexo indireto. Exemplos
  `M_Velvet` e `M_SoapFilm` na cena materials.
- **Fora (ainda sem efeito):** Diffuse Roughness, Coat Normal, Subsurface IOR e Anisotropy; os pinos de lóbulo ligados
  a nós/texturas.
- **Custo:** zero para quem não usa (as variantes de sempre iguais, `shader_spill` passa: `mesh` 95 sem spill, `mesh_rt`
  e `mesh_rt_t` 128 com 16 B, como de manhã). Com lóbulos: `mesh_rt_x` 128 com 48 B, `mesh_x` 80 com 48 B; criadas em
  ~90 ms (com o cache frio, ~1,1 s) no quadro em que aparecem.
- **Exemplos** (Content/Materials/Examples, feitos quando faltam): `M_CarPaint` (coat), `M_BrushedMetal` (anisotropia),
  `M_Skin` (subsuperfície), `M_Glass` (transmissão), na segunda fileira da cena `materials`.
- **Testes:** `mat_pins` (a tabela; o Coat tomado do nó; um Principled simples sem lóbulos; a Transmissão; o F0 da água,
  IOR 1,33 → 0,020), `material_lobes` (Épico) e `material_lobes_low` (Baixo): as variantes criadas e usadas, com validação.

## Specular anti-aliasing (M2)
Já existia: `mesh.frag` alarga o lóbulo pela variação da normal no pixel (Kaplanyan e Hoffman 2016, `a² += min(½(|∂n/∂x|² +
|∂n/∂y|²), 0,18)`), em todos os presets. O plano pedia isso; a nota não dizia. Falta só o Toksvig nos mips do normal map
(o bake gera mips por blit: precisaria de um passe de compute).

## Cor por vértice e normal de detalhe (M5, 2026-11-09)
- **Cor por vértice**: o glTF traz o `COLOR_0` (bytes normalizados ou floats, VEC3 com alfa 1), guardado por canto no
  `TMesh.col` e no `TriOut.col` como RGBA8 com a cor em sRGB. O `mesh_build_corners_col` solda os cantos considerando
  a cor (`meshopt_generateVertexRemapMulti`), o `tag` leva cada vértice ao canto de origem e os vértices próprios dos LODs
  pegam a cor pela posição. Fica no `.rmesh` **v4** (`RMESH_COLORS`: um uint32 por vértice no fim; v1 a v3 continuam
  abrindo). No renderer as cores vão para as mesmas palavras dos morph targets (`R.morphs`), com `GpuMeshInfo.vcol_first`
  (144 B agora, com padding explícito) e `FrameBuf.mesh_words`. Quem lê é o `mesh_x.vert` (o `mesh.vert` com
  `-DRE_EXT`), só nas pipelines RE_EXT, que passa a cor linear em `location 5`. As outras pipelines não pagam nada.
- **Nó Color Attribute** (o `ShaderNodeVertexColor` do Blender, id 133): no bake ele é branco, e o `mesh.frag` RE_EXT
  multiplica a Base Color pela cor. `mat_vcol_usage` só aceita os usos em que isso é exato: Color → Base Color, direto ou
  como o A de um Mix Multiply (qualquer fator) ou o B com fator 1. Fora disso (Alpha, outros nós) o nó ganha o "!"
  laranja e o Details avisa.
- **Normal de detalhe** (o detail texturing da Unreal): Details do material > **Normal de Detalhe**: Material de Detalhe
  (arrastado do Content Browser), Repetição e Força. É a normal **já assada** de outro material, repetida mais fina,
  combinada por whiteout sobre a normal do material. O slot vai no `e6` da `GpuMatExt` (agora 7 vec4, 112 B), e o
  `detail_follow` (no `mat_update`) acompanha o slot do material de detalhe sem reassar quem o usa. Slot e flags vão como
  valores float, não como bits: os bits de um inteiro pequeno são um denormal, e a GPU zerava esse valor na leitura.
- `MATX_LOBES` / `MATX_VCOL_TINT` (`mat_ops.h`, no `e6.w`): um material só com M5 entra no RE_EXT mas usa o BRDF comum
  (sem `g_ext`).
- **Limitação, e um bug achado**: as runs de uma malha são opacas, masked ou translúcidas como um todo. Uma malha que
  também tem um material Masked, Two Sided ou Translucent é desenhada pelas pipelines masked, que não têm RE_EXT. Assim os
  lóbulos e o M5 dela não aparecem. A cena `materials` tinha o M_Glass numa esfera básica, o que tirava os lóbulos de todas
  as esferas (M1b, M3, M4). O vidro foi para um cilindro, e o render avisa uma vez por malha
  (`aviso: mesh ... has Masked, Two Sided or Translucent materials too`).
- Exemplos: `M_VertexColor`, `M_DetailNoise` (ruído → Bump) e `M_DetailPlaster` (detalhe ×12), mais
  `Meshes/Examples/SM_VertexColorSphere.rmesh` (arco-íris escurecendo para baixo, gerada na cena), na terceira fileira
  da cena `materials`.
- Testes: `mat_pins` (os usos do Color Attribute), `import_glb` (o `test.glb` com `COLOR_0` no Tri: "1 with vertex
  colors" no relatório), `materials` / `material_lobes` (a esfera colorida carregada).
- Medida (RTX 3060, Épico, cena materials com o M5 e os lóbulos ativos nas esferas): 301 fps, GPU 3,1 ms. O normal de
  detalhe custa uma leitura de textura a mais, só nos materiais que o usam.
- Falta: pintar a cor por vértice no editor (o Mesh Paint da Unreal); cor por vértice no FBX e no `.tlend`, nos skins e
  nas pipelines masked; o Color Attribute como fator de mistura entre dois materiais (camadas).

## Decals (M6, 2026-11-09)
O Decal da Unreal, num renderer forward: **decals em tiles**. Um material é projetado ao longo do X do ator sobre as
superfícies opacas dentro da caixa (semi-eixo de 100 cm × escala, como os volumes) e entra no lugar da cor, da normal,
da rugosidade e do metálico **antes da luz**. Assim o decal recebe luz, sombra e GI como a superfície.
- `CDecal` (`COMP_DECAL`): Material do Decal (qualquer `.rmat`: a cor assada, o Alpha como a parte, a normal, a rugosidade
  e o metálico), Opacidade, Fade pelo Ângulo (padrão 75°), Distância do Fade, Tempo de Vida e Desaparecer (marcas de bala,
  sangue) e Ordem. No editor: Adicionar > Efeitos Visuais > Decal (apontado para o chão, 2 × 2 m, o M_DecalGrime). A caixa
  e a direção da projeção aparecem com o ator selecionado.
- `decal.c`: `decals_gather` monta a cada quadro os `GpuDecal` (112 B: mundo → caixa, o retângulo na tela a partir dos 8
  cantos, os eixos, o slot do material, o cosseno do fade e a opacidade já com os fades de vida e distância). São
  ordenados pela ordem e depois pelo tamanho (o maior embaixo), com até 1024 (passou disso, ficam os mais perto).
  `decals_update` envelhece e apaga os que têm tempo de vida (jogando). `decal_spawn` é usado pelo nó.
- `decal_bin.comp`: uma thread por tile de 16 px do render lista até 31 decals que tocam o tile. No `mesh.frag` (RE_EXT)
  cada pixel lê o próprio tile, testa a caixa e aplica o decal: fade pelo ângulo com a normal, fade nos 20% finais da
  profundidade da caixa, e o Alpha assado. As texturas são lidas com `textureGrad`, com gradientes tirados de
  `dFdx(i_world)`, porque o laço não é uniforme. A normal do decal vai no espaço dele (U ao longo do Y, V para baixo no Z).
- Com algum decal na tela as pipelines RE_EXT entram, igual aos lóbulos. Por isso valem as mesmas limitações: as runs
  Masked ou Translucent de uma malha e os translúcidos (OIT) não recebem decals.
- Nó **Spawn Decal at Location** (Location, Rotation, Decal Size em semi-eixos, Life Span; o material no Details),
  como o da Unreal. Junto vieram os **Set/Get Render Style**, que estavam declarados sem execução na VM desde o A9.0.
- Teste `decals` / `decals_low` (validação, Épico e Baixo: "Decals: 6 of 6 drawn"). `REALENGINE_NO_DECALS=1` desliga, para
  medir.
- Medida (RTX 3060, Épico, cena `decals`, 6 decals): GPU 3,11 ms contra 3,06 sem eles (cerca de 0,05 ms). O passe
  principal só paga onde há decal no tile.
- Falta: Receives Decals por malha (personagens), decals nos Masked e nos translúcidos, decal por Blueprint preso a um ator
  (Spawn Decal Attached), DBuffer para mais de 31 decals sobrepostos num tile.

## Parallax occlusion (M7, 2026-11-09)
- **O que o artista faz**: no material Opaque, liga um nó **Displacement** (o do Blender: Height, Midlevel, Scale) na
  entrada **Displacement** da Material Output. Também vale ligar direto uma textura (vira a média do RGB).
- **Bake**: a altura vai para o **alfa da textura de cor**, que os materiais opacos não usam. A op nova `MOP_SET_W`
  (`mat_ops.h` / `mat_vm.glsl`) troca o alfa do closure pela altura. Não há textura nova nem leitura a mais nos materiais
  sem parallax. A profundidade é Scale × 0,05 em unidades de UV (até 0,5) e vai no `e7` da `GpuMatExt` (agora 8 vec4,
  128 B), junto com o máximo de passos (32). A flag é `MATF_POM`, que liga o RE_EXT.
- **Shader** (`mesh.frag` RE_EXT, antes das leituras do material): um ray-march em espaço tangente ao longo da visão. São 8
  passos olhando de frente e até 32 de lado, depois a interpolação entre os dois últimos. A cor, a normal, a rugosidade e o
  emissivo são lidos no UV onde ele chega. Os gradientes vêm do UV original (`textureGrad`), porque o laço não é uniforme.
  Materiais animados (flipbook) ficam fora.
- Exemplo **M_ParallaxCobble**: dois Voronoi (F2 − F1 × 5, zero na borda das células) vão para o Displacement e para um
  Bump. É um calçamento com o rejunte afundado, numa laje na cena `decals`.
- Medida (RTX 3060, Épico, o calçamento cobrindo metade da tela): passe principal 2,0 ms contra 1,86 no chão comum na
  mesma vista (cerca de 0,15 ms).
- Falta: a silhueta e a profundidade do pixel (o parallax não recorta a borda da malha nem desloca a sombra), sombra
  própria dentro do relevo. O microdeslocamento de verdade veio no Q17 (Enable Tessellation: tessellation.md): a mesma
  altura move os clusters da DAG; com ele ligado o material não faz parallax.

## Editor de material: zoom, busca, pinos (2026-11-10)
- O texto dos nós acompanha o zoom (antes ficava gigante longe e minúsculo perto). Busca no menu de nós (palavras, pt/en).
- O Principled BSDF mostra todas as entradas como pinos. Os lóbulos extras valem em Opaque, Masked e Two Sided (as corridas
  masked têm RE_EXT desde 2026-11-10: notas-dev/render.md); só no Translucent ficam cinza. Num Masked a Transmissão não
  o torna vidro (o recorte vence).

## Superfície molhada e suor: o nó Wet Surface (2026-11-16)
Pedido: "colocar um sistema de shading molhado como se fosse suor, ou mesmo molhado, e ter muitas opções de uma forma bem
interativa ao usuário no gráfico de materiais".
- **Nó** `NODE_RE_WET` (258, "RealEngineWetSurface", categoria Shader): Shader entra e sai igual (compilado direto; o
  `principled_ext` atravessa ele até o Principled). Pinos: Wetness, Porosity (−1: pela rugosidade, `rain_porosity`),
  Water Film, Film Roughness, Droplets, Droplet Size (mm), Running, Sweat; propriedade Máscara (Tudo, Cor do Vértice R,
  Virado para Cima, Manchas por ruído no espaço da peça).
- **Constantes** (`wet_ext`): `GpuMatExt` passou de 8 para 12 vec4 (192 B): e8 molhado, porosidade, película, alfa da
  película; e9 gotas, tamanho, escorrer, suor; e10 índices + 1 dos parâmetros; e11.x a máscara. `MATX_WET` liga o
  `MATF_EXT` (variantes RE_EXT; `mat_ext` sabe de `m->wet`).
- **Dinâmico sem bake**: Wetness, Water Film, Droplets e Sweat ligados a um Value com Parameter Name viram parâmetros do
  material que o bake não lê (`c->nbaked`: os do programa vêm antes). `mat_set_param` (Set Scalar Parameter Value on
  Materials) grava o valor e não marca o bake quando `k >= nbaked`; o `mesh.frag` lê `f.mat_params[param_off + i]`.
- **Shader** (`water.glsl mat_wet`, no bloco do material das RE_EXT opacas): as duas camadas da chuva (base mais escura
  e saturada pela porosidade, menos com suor; lóbulo próprio mais liso; película `rc_set` com normal entre a do relevo e
  a da face), gotas e rastros pelo `rain_drops_d` **no plano do eixo dominante da normal no espaço da peça** (pela normal
  do pixel, numa superfície curva as células giravam com ela e viravam ruído: o primeiro teste não mostrava gota
  nenhuma), borda escura das gotas (mais rasa no suor). Chovendo, o molhado da chuva vem por cima.
- **Exemplos**: `M_WetConcrete` (concreto poroso, gotas escorrendo) e `M_SweatySkin` (pele com o parâmetro "Sweat"),
  fileira da frente da cena `materials`.
- **Medida** (RTX 3060, 1440p Épico, cena `materials` com a câmera no concreto molhado, ~60% da tela, mínimo da GPU, 2
  rodadas): passe principal 2,164/2,153 ms sem (`REALENGINE_NO_WET=1`) contra 2,554/2,567 com: **+0,4 ms** nesse
  enquadramento; zero para quem não usa.
- **Testes**: `mat_pins` (o coat atrás do Wet Surface, as constantes, o parâmetro Sweat), `materials`, `material_lobes`.
- **Falta**: no Baixo (RE_LSM sem RE_EXT) o material aparece seco; máscara por textura; Wet Surface em translúcidos.

## Transmission real: Refrativo e Fino, absorção, sombra colorida (2026-11-17, plano 0.43 Q4b)
- **Material** (Details > Transmissão, `mat_editor.c`): **Transmissão** (o pino Transmission Weight do Principled,
  editado ali quando solto), **Modo** (`transmission_mode`: Refrativo / Fino), **Cor e Distância da Absorção**
  (`absorption`, `absorption_dist`) e **Sombra Colorida** (`colored_shadow`, padrão ligado). JSON: `transmission_mode`,
  `absorption_r/g/b`, `absorption_distance`, `colored_shadow` (os antigos abrem como Refrativo, claro, com sombra).
- **Parâmetro (Material Instance, Blueprint, nó):** um **Value com Parameter Name** no pino Transmission Weight vira um
  parâmetro lido pela GPU na hora (como o Wet Surface: `transmission_of` + `param_index`, fora do bake); a instância o
  sobrescreve e o `Set Scalar Parameter Value on Materials` muda sem recompilar. Antes um pino ligado valia 0.
- **Refrativo** (`MATF_TRANSLUCENT`, passe OIT): a opacidade × (1 − peso), o peso do parâmetro se houver (`e8.w`). O
  fundo visto através dele ganha a **cor base** pelo peso e a **absorção** de Beer-Lambert pela espessura da refração
  (mais longa de lado): `g_tint`. O alvo da revelação do OIT (`split_sum`) passou de RG16F para **RGBA16F**: r a
  revelação de sempre, gba o produto das transmitâncias coloridas (mistura ZERO / ONE_MINUS_SRC); o
  `shadow_split.comp` passe 3 multiplica o fundo por elas. A rugosidade (fosco) e a refração/dispersão já existiam.
- **Fino** (`e11.w`): o material fica **opaco** (sem OIT); no `brdf_base` a parte `tw` da difusa atravessa: a luz de
  **trás** (`max(-N·L, 0)`) ilumina a frente na cor do material, a frente fica com (1 − tw). A sombra do sol de um
  ponto iluminado por trás é lida do lado de lá (`shadow_factor(-n)`). Só nas variantes RE_EXT (o material liga os
  lóbulos: `MATX_LOBES`).
- **Sombra colorida (sol):** as instâncias Translucent entram no TLAS com a máscara própria `RT_MASK_GLASS` (0x20;
  antes máscara 0), que nenhum outro raio usa (`tlas_fill.comp`, dinâmicas no `render.c`). Nas variantes RE_EXT com raio,
  com `GI_GLASS_SHADOW` (algum vidro com Sombra Colorida enviado: `mat_glass_shadow`), a luz do sol de cada pixel é
  multiplicada por `glass_tint`: um raio até o sol que soma as faces de entrada (até 6) de cada vidro × a tinta dele
  (`e9`: a cor base média × a absorção pela espessura).
- **Sombra colorida sem raio** (Baixo, GPU sem ray query, raios por software; 2026-11-17, "resolva as limitações"): os
  vidros com Sombra Colorida viram **caixas orientadas** (`glass_note` guarda a caixa da malha × as rows por entidade ao
  montar a instância; `glass_fill` manda as 16 mais perto da câmera em `GpuFrame.glass`, o número em `rgi_info.y`). No
  `mesh.frag` (RE_EXT e RE_LSM sem raio, e RE_SWRT) `glass_box_tint` testa o raio do pixel até o sol contra cada caixa
  (slab) e multiplica pela tinta (`e9`). Medida (`--scene glass --scalability 0`, Baixo): 3,273/3,286 ms sem
  (`REALENGINE_NO_GLASS_SHADOW=1`) contra 3,285/3,291 ms com: **+0,01 ms**.
- **Cena `glass`** ganhou o painel âmbar (Refrativo, absorção 0,95/0,55/0,12 em 2 cm, sombra colorida) e a cortina
  vermelha (Fino, parâmetro "Transmission" 0,6, Two Sided); o teste `translucent` falha se o âmbar não for vidro com
  sombra ou a cortina não for opaca com lóbulos.
- **Medidas** (RTX 3060, 2560×1417, Épico, `--scene glass`, mínimo da GPU, 2 rodadas): antes (main) 5,028/5,069 ms;
  depois 5,386/5,463/5,420/5,512 ms (+0,35–0,45 ms: os dois atores novos cobrindo a tela e o alvo RGBA16F do OIT);
  `REALENGINE_NO_GLASS_SHADOW=1` 5,382/5,522 ms: **o raio da sombra colorida ~0**.
- **Testes:** `translucent` (Épico, render) e `transmission_medium` (Médio, materiais). Artigo `transmission`.

## Interior Mapping: salas atrás das janelas (2026-11-17, plano 0.43 Q4d)
- **Nó** `Interior Mapping` (`RealEngineInteriorMapping`, `NODE_RE_INTERIOR` 257, categoria Shader): entre o Principled
  (a fachada) e o Material Output, como o Wet Surface (passa o shader adiante; `principled_ext` e `transmission_of`
  o atravessam). Entradas: Rooms U/V, Room Depth, Frame, Furniture, Curtains, Night, Lit Rooms, Light Color, Light
  Strength, Seed; o campo de imagem é o **atlas** (Colunas/Linhas nas propriedades, 1..8).
- **GPU** (`interior_ext` → `MATX_INTERIOR`, `e8`-`e10`, `e7` o atlas, `e11.x` o índice do parâmetro Night): no
  `mesh.frag` (RE_EXT, opaco) `interior()` divide o UV em janelas; em cada uma o raio da vista entra numa caixa de
  `Room Depth` larguras (Van Dongen 2008) e acha a parede do fundo, as laterais, o piso ou o teto. Com atlas: o tile da
  sala (sorteado) em perspectiva (o fundo na metade do meio); sem atlas: parede colorida por sala, piso de madeira com
  tábuas, teto claro, cantos mais escuros. **Móveis** numa camada a 60% da profundidade (sofá, mesa ou estante por
  sala), **cortinas** logo atrás do vidro (dos dois lados, quanto por sala, com dobras). Luz: o dia entrando (o céu na
  normal + um piso relativo à exposição: a luz rebatida que as sondas não veem numa caixa que não existe) ou, à
  **noite**, uma lâmpada no teto de uma parte sorteada das salas (algumas trocam a cada 30 s). A moldura fica com o
  shader da fachada; na janela a superfície vira vidro (escuro e liso: o reflexo do céu) e a sala é a emissão através
  dele (× 1 − Fresnel). Sem NaN nem infinito para fora (`room` limpo).
- **Luz do dia física** (2026-11-17, no lugar do piso pela exposição): um **fator de luz diurna** de 5% da iluminância
  horizontal de fora (o céu `sh_irradiance` para cima + cada luz direcional × o seno da altura), menos fundo adentro.
  As salas de dia aparecem com profundidade e móveis; à noite, sob a exposição de um dia claro, as lâmpadas (~120 nits)
  ficam escuras como na vida (numa cena de noite a exposição sobe e elas acendem).
- **Atlas de exemplo:** `Materials/Examples/Textures/T_RoomsAtlas.png` (gerado na primeira abertura: 4 × 4 salas de
  256 px, cada uma vista da janela: parede do fundo com quadro, janela ou estante; sofá, cama ou mesa; piso de madeira
  ou tapete; teto com luminária; porta numa lateral) e o material `M_BuildingAtlas` (Interior Mapping 4 × 4 com o
  parâmetro Night). O atlas é lido em sRGB → linear. A cena `interiors` usa os dois: por dia salas geradas e atlas, à
  noite os dois.
- O `Night` ligado a um Value com Parameter Name muda por Material Instance e Blueprint sem recompilar (`e11.x`).
- **Cena `interiors`**: quatro prédios de 12 × 30 m (4 × 10 salas de 3 m por face), dois de dia e dois à noite.
  `REALENGINE_NO_INTERIOR=1`: só as fachadas (o "antes").
- **Medidas** (RTX 3060, 2560×1417, Épico, mínimo da GPU, 2 rodadas): sem 2,805/2,831 ms; com 3,295/3,325 ms: **+0,49 ms**
  com as fachadas cobrindo quase a tela toda (inclui passar às variantes RE_EXT). Zero para quem não usa.
- **Teste:** `interior_mapping` (área `materiais`). Artigo `interior-mapping`.
- Medida depois do atlas e da luz física: 2,825/2,901 ms sem, 3,309/3,340 ms com (**+0,46 ms**).
- Falta: a luz das salas acesas na calçada à noite (um ponto de luz por janela seria caro; um lightmap da fachada).

## UDIM (Q18, 2026-11-18)
Texturas UDIM (um tile por quadrado da UV) no Image Texture, com bake e streaming por tile: notas-dev/udim.md.

## Retrorrefletor e flocos metálicos (Q27.8, 2026-11-23: os materiais da cidade à noite do GTA 6)
- **Retroreflection** / **Retroreflection Color** (`Material.retro`, `retro_color`; `.rmat` `retro`, `retro_r/g/b`): o
  lóbulo do Eye Glow (Q13) em qualquer Shading Model menos Eye e Toon. Empacotado em `e5.w` (força × 16 nos bits de cima,
  RGB565 embaixo) e ligado pelos lóbulos (`MATX_LOBES`); `mesh.frag` põe `g_retro` no bloco dos lóbulos e o `brdf()` soma
  `cor × força × (l·v)^40` por luz: pico quando a luz vem de junto do olho. Sem retro, o `e5.w` é o IOR do vidro (< 2^16):
  não aciona o ramo.
- **Bug achado (Q13 também)**: `uint(e5.w + 0.5)`. O número empacotado passa de 2^23, onde o float anda de 1 em 1: o
  +0,5 arredondava para o par e zerava a cor (0x80FFFF virava 0x810000). Com força ≥ 8 o Eye Glow não brilhava. Agora
  `uint(round(e5.w))`.
- **Metallic Flakes** / **Flake Size** (cm) / **Flake Spread** (`flakes`, `flake_size`, `flake_spread`): um `vec4 e12`
  novo no `GpuMatExt` (13 vec4 por slot; a tabela do Toon depois dele acompanha pelo `sizeof` e pela struct do GLSL).
  No `brdf_base`, antes do verniz: cada célula de Flake Size tem um floco (70% delas, por hash pcg3d), inclinado por
  Flake Spread, com um GGX afiado (0,06). Pela pegada do pixel (`fwidth(i_world)`, tirada no começo do `main`, fora de
  fluxo divergente) as células se juntam em potências de 2 com mistura entre dois níveis (sem pulo ao afastar): o brilho
  cai como 1/√ dos flocos por célula e entra um lóbulo largo com a média deles. Multiplicado pelo F0 da tinta.
- **Exemplos**: `M_RoadSign` (retro 8), `M_Reflector` (vermelho, retro 10), `M_CarPaintFlakes` (azul metálico, coat,
  flocos 1,5 de 0,4 mm).
- **Medidas** (RTX 3060, 1440p, cena `city_night`: placa a 12 m, farol de 20 000 cd ao lado do olho): a placa com o
  filme **1140 nits no pico, 6,2 de média do quadro**; com Retroreflection 0 (em memória) 41 nits e 0,47: **13× na
  média**. Quadro 1,8 ms (550 fps), passe principal ~1,1 ms; `shader_spill` sem vazamento.
- Fica para depois: reflexo planar em resolução reduzida (espelhos e interiores sem RT), política de raio por superfície
  (água pequena e carros perto por raio, oceano por SSR e céu), faróis distantes como sprites, exposição da noite
  (a automática estoura a placa: EV100 −2,3 na cena).

## Material Editor: soltar link no vazio, Detalhes reorganizados (2026-11-23)
- **Soltar um link no vazio** abre o menu de adicionar só com os nós que recebem aquele tipo (o Shader só em Shader) e
  liga o escolhido (a entrada do mesmo tipo, senão a primeira que serve), como o link search do Tlender e o menu
  sensível ao contexto das Blueprints. **De trás para frente**: arrastar de uma entrada livre e soltar no vazio cria o nó
  que a alimenta (posto à esquerda). Um link tirado de uma entrada e solto no vazio só se desfaz.
- **Detalhes**: busca no topo (filtra seções e entradas, pt e en), abas **Material** e **Nó** que seguem a seleção,
  seções recolhíveis (as em uso abrem sozinhas), entradas ligadas mostram de qual nó vêm (clique: vai até ele); a dica do
  "Two Sided" estava no item errado.
- O preview libera as texturas do bake antigo 4 quadros depois (antes: liberadas em uso, VUID-vkFreeDescriptorSets-00309).
- Teste `mated_ui` (`src/mat_editor_test.inc`, mouse e teclas injetados): arrasto em sincronia; soltar do BSDF no vazio
  + "mix shader" Enter = Mix Shader ligado; soltar da Cor Base no vazio + "image texture" Enter = Image Texture ligada.
  Artigo `material-editor-flow` no Assistente.
- **Emissive Light** (Detalhes > Material, 0-4, padrão 1): quanto o brilho do material ilumina a cena (notas-dev/luz.md);
  **Emissive Shadows** (desligado por padrão, ~1 ms) liga a sombra por raio dessa luz; o painel mostra a emissão média em
  nits.

## Texturas soltas no Principled ligadas pelo nome (2026-11-23, pedido do usuário)
- `mat_map_slot` (material.c): o nome da textura em palavras (separadores e CamelCase), a resolução tirada do fim (2k,
  4096, 1024px), as duas últimas palavras juntas e depois a última contra os finais conhecidos (BaseColor, Albedo,
  Diffuse, Color, D; Normal, NormalGL, Nrm, N; NormalDX; Roughness, R; Gloss; Metallic, M; ORM/ARM; Emissive; Opacity,
  Alpha, Mask; AO; Height); devolve também a chave (o nome antes do final).
- `mat_editor_drop_textures`: soltar no Principled BSDF liga cada uma (Base Color sRGB; Metallic, Roughness, Gloss por
  Invert, ORM por Separate Color, Normal por Normal Map com DirectX quando NormalDX, Emission com força ≥ 1, Opacity com
  o material Masked) em coluna na ordem das entradas, os ajudantes entre elas, AO/Height/desconhecidas embaixo sem
  ligar; os mapas de dado como Não-Cor. No vazio: uma coluna de Image Textures. Teste `mat_autolink` (18 nomes de
  Substance/Quixel/Poly Haven/Unreal e 8 texturas soltas num Principled).
- A importação usa a mesma ideia pelo nome do material (importar-tlender.md, `mc_auto_maps`).

## Configurações da textura (2026-11-23, pedido do usuário)
- `tex_settings.c/.h`: arquivo oculto ao lado da imagem (`.<arquivo>.rtex`, JSON versionado, como o `.mat` das malhas):
  espaço de cor (sRGB / Linear / Normal Map), compressão (BC7 / nenhuma), filtro e repetição (os do nó Image Texture),
  tamanho máximo. Sem arquivo, pelo nome (`mat_map_slot`: dados → Linear, normal → Normal Map, o resto sRGB): toda
  importação já chega organizada sem gravar nada.
- O bake (`material_gpu.inc mat_source_texture`) lê a origem reduzida pela metade até caber no tamanho máximo (caixa
  2×2) e sem BC7 quando "Sem Compressão"; os nós criados por arraste e pela ligação pelo nome usam
  `tex_settings_to_node`. O visualizador de textura tem o painel (salva na hora; "Aplicar aos Materiais" percorre os
  `.rmat` do projeto e acerta os nós que usam a textura) e mostra a memória lida pelos materiais.
- Teste: `mat_autolink` confere os padrões pelo nome e o salvar/ler; o painel conferido por print (`--open-asset`).

