# Estudo de desempenho (2026-11-13, projeto MyProject)

Pedido: "uma engine estável, de alto desempenho... bater mais de 200 FPS no Realista e mais de 100 FPS estáveis no
Vídeo-realista, sem upscaling", com o MyProject (FirstPersonMap: sala com caixas, 2 luzes pontuais, sol, céu com
nuvens volumétricas e neblina, e o tigre-robô `Tiger_RFY_FastWalk` andando perto da câmera do Play). Queixa do usuário
no meio: "se eu me aproximo muito de um personagem cai muito".

## Como medir (sem upscaling)
- `--upscale-mode 1` (Nativo: o TSR só como antialiasing, 100%): o projeto tem resolução dinâmica ligada (meta 240
  fps), que baixaria a resolução sozinha. `--play --scalability 3 --render-style 1|3 --bench 5 --budget 100 --gpu-passes`.
- Os números confiáveis: **GPU mínima** (o quadro mais barato: o custo dele sozinho na GPU) e as fatias do `--budget`.
- **GPU ocupada por outras sessões** (benchmarks de RT do Tlender, `tlender_tests bench-rt-hot`, `fur`) deixa tudo
  2-20× mais lento: o script de medida espera a GPU abaixo de 15% por 12 s antes de cada medida.

## O diagnóstico (RTX 3060, 2560×1417, Épico, nativo)
Antes: Realista **78 fps** (GPU mínima 11,5 ms), Vídeo-realista **40 fps** (22,7 ms). Estável no regime (o pré-passe
fica ligado; sem travadas periódicas), então é custo puro.
- O tigre: **309 mil triângulos, 212 mil vértices, um LOD só** (as malhas com esqueleto não ganhavam LODs), metálico e
  liso. Perto, cobre ~35% da tela e **cada pixel dele traça raios**: sombra do sol (o híbrido da VSM), as 2 luzes
  pontuais, o reflexo (metal liso) e, no Vídeo-realista, a GI difusa + especular do denoiser, com mais um raio de
  sombra em cada acerto sem cache de superfície (a própria pele do tigre). O custo cresce com a área na tela.
- O shader do passe principal com RT: **128 registradores e derramando para a memória local** (16 a 112 bytes):
  ocupação ~33%, cada warp espera os raios.
- Passe principal no Realista (8,0 ms): base sem raios 3,5; reflexo ~1,6; raio híbrido do sol ~0,7; sombras das luzes
  pontuais + a variante RT ~2,2. No Vídeo-realista (19 ms): +11 ms da variante do denoiser.
- "Scene" (1,8 ms): o skinning custa 0,1 ms; o resto é o refit da BLAS de 309 mil triângulos, a TLAS, as nuvens e a
  neblina (o `weather_record` cai dentro desse intervalo).

## O que entrou
1. **LODs para malhas com esqueleto** (`mesh_skinned_lods`, mesh_build.c): simplificadas sobre as posições soldadas
   (`meshopt_generateShadowIndexBuffer`: as costuras de UV/normal travavam a simplificação como bordas), nos vértices do
   LOD 0 (o skin.comp dobra só esses; nenhum vértice se move, os pesos ficam certos). As peles importadas antes ganham
   os LODs ao carregar, em cache por conteúdo (`%016llx_sl1.bin`: 689 ms → 6 ms). O tigre: 4 LODs, 309 k → 3,3 k.
2. **BLAS do ray tracing no LOD 1** (`skin_rt_lod`, como o ray tracing LOD bias da Unreal): o refit e os raios num quarto
   dos triângulos; o `ids.z` da instância aponta o LOD da BLAS (os acertos), o raio sai com o erro do LOD
   (`MESH_INFO_RT_LOD_SHIFT`, mesh.vert). Pele Masked (cards de cabelo) fica no LOD 0. `REALENGINE_SKIN_RT_LOD=0|1|2`.
   Passe principal: Realista 7,76 → 6,73 ms; Vídeo-realista 17,9 → 14,6 ms. Imagem sem acne de auto-sombra.
3. **Checkerboard do denoiser** (`GI_DEN_CHECKER`, do Baixo ao Épico; o Cinemático traça os dois): um pixel que precisa
   dos dois raios traça um por quadro (xadrez por pixel, troca a cada quadro); o sinal que faltou vai como
   `DEN_MISSING` e o pré-blur do `denoise.comp` completa pelos vizinhos (o centro e os taps sem o sinal não entram; sem
   nenhum, o histórico segue sem somar quadro). −0,9 ms; cintilação igual (7,95% → 7,83%). Xadrez em blocos de 2, 4 e
   8 px foi medido: igual (o custo não é a divergência). `REALENGINE_DEN_CHECKER=0|1`.
4. **Cópia do SSR só quando ele roda:** a cor e a profundidade eram copiadas todo quadro para o SSR do quadro
   seguinte, mesmo com o reflexo por raio ligado (Alto/Épico com RT: o SSR nunca roda). Agora só sem o reflexo por raio
   (o primeiro quadro sem ele copia, o seguinte usa). 0,15-0,3 ms a 1440p.
6. **Raios da GI do denoiser em compute** (`den_trace.comp`, depois do passe principal e antes do `denoise.comp`):
   o `mesh.frag` (RE_DEN) só empacota no `o_den` o que os raios precisam (x: a luz difusa do ponto, pré-exposta, a do
   erro; y: o céu que o ponto vê em meio float + a visibilidade em 8 bits, o viés do raio em 6 bits como raiz dos cm
   em quartos, `DEN_RAY_D`/`DEN_RAY_S`); a normal e a rugosidade já iam na chave, a posição sai do `o_guide.z` pela
   `tsr_inv` (a inversa da matriz com jitter: o ponto exato do fragmento). O compute traça com os registradores só dele
   (mesma amostragem R2, mesmo checkerboard, mesma luz no acerto) e escreve x, y e o alfa de w. Malhas do passe
   principal no Vídeo-realista 11,7 → 6,7 ms; os raios no compute 1,9 ms; imagem igual (casa no Fotorrealista:
   diferença média 1,3/255). Os testes do denoiser passam com validação.
5. **GPU por passe** (`--gpu-passes`, Debug Viewer > GPU Passes; notas-dev/debug-viewer.md): o `stat gpu`.

## Medidos e descartados
- BLAS da pele com PREFER_FAST_TRACE ou refeita todo quadro: o build custa 2 ms a mais e os raios não ficam mais rápidos.
- Sem o raio de sombra no acerto da GI (sombra do ponto atingido): −0,3 ms, não paga a luz vazando nas dobras.
- Xadrez do denoiser em blocos (warps coerentes): o mesmo ganho do xadrez por pixel.

## Antes × depois (mesmo momento, GPU livre; 0.30 = build-release × este)
| MyProject, Épico, 1440p nativo | 0.30 | agora |
|---|---|---|
| Realista | 80 fps, GPU mín. 11,6 ms | **90 fps, 10,1 ms** |
| Vídeo-realista | 45 fps, 22,1 ms | **65 fps, 14,8 ms** |
| Realista no Alto (o preset do projeto) | 94 fps, 9,8 ms | 106 fps, 8,8 ms (medido antes do item 6) |
- (Uma medida anterior deu 98 fps no Realista; repetida depois, com o build anterior recompilado, dá 10,2 ms: a de
  10,1 ms acima é a que se repete. A 0.30 mediu o mesmo nas duas vezes.)

Perfil por passe do Vídeo-realista agora (ms): passe principal 11,7; denoiser 3,1; nuvens na tela 1,0; cache de
superfície 0,8; composição 0,34; neblina 0,33; skin + BLAS 0,24; GTAO 0,22; o resto < 0,2 cada.

## Por que ainda não dá 200 / 100 fps nesta cena, e o que falta (em ordem de ganho)
As metas pedem 5 ms (Realista) e 10 ms (Vídeo-realista) com um personagem metálico de 309 mil triângulos ocupando um
terço da tela, raios por pixel, nuvens volumétricas e neblina, a 1440p nativo numa RTX 3060. Sem raio nenhum
(`--no-rt`) o quadro já é 7,1 ms: o caminho passa por mudar a arquitetura dos raios, não por ajuste fino.
1. **Raios fora do fragment shader**: feito para a GI do denoiser (item 6). O reflexo do Realista foi medido: custa
   ~1,0 ms no passe de malhas (`REALENGINE_NO_RT_REFLECTIONS=1`: GPU mínima 10,2 → 9,45 ms); num compute custaria ~0,5 ms
   mais um alvo novo no passe principal (~0,15 ms de escrita), ~0,3 ms líquidos: não feito. O que sobra no fragment
   shader com RT são as sombras (o sol híbrido e as luzes pontuais) e esse reflexo. Próximo: a GI do denoiser a meia
   resolução (o compute já permite; reconstruída pelo filtro).
2. **Denoiser mais barato** (3,1 ms): pré-blur e pós-blur com memória compartilhada (as 8 leituras de 16 B por tap
   viram uma leitura por pixel do bloco) e o histórico em meia resolução para o difuso.
3. **Volumes** (nuvens na tela 1,0 + neblina 0,33 + panorama 0,25): reaproveitar mais quadros no Alto/Épico, meia
   resolução da neblina por froxel.
4. **Raio de reflexo no Realista** (~1,6 ms no tigre metálico): meia resolução + reconstrução temporal, ou SSR primeiro e
   raio só onde a tela não tem a informação (o híbrido da Unreal).
- O teste `pause_options` e a travada do `world_partition` falham também na 0.30 (não são destas mudanças).

## Depois dos raios em compute (2026-11-14)
Perfil do Vídeo-realista no MyProject (Épico, 1440p nativo, `--gpu-passes`, os três passes do denoiser separados):
malhas do passe principal 7,7 ms; denoiser: pós-blur e cor 1,3 (inclui o que vem até a próxima marca), raios da GI
1,1, pré-blur 0,64, histórico 0,49; cache de superfície 0,9; nuvens na tela 0,87; composição 0,33; neblina 0,32;
chuva 0,27 (o projeto ganhou uma Rain); skin + BLAS 0,24.
- Próximo no denoiser: pré-blur e pós-blur com memória compartilhada (cada tap lê 16-28 B espalhados).
- Céu: no Épico (nuvens de tela em meia resolução) aparecem arcos finos de elevação constante, que no Médio/Alto são
  estrias suaves: estrutura do campo de nuvens revelada pela resolução maior (não são os cirros: mudar o fbm deles não
  mudou nada). Precisa de uma investigação própria (mapa de cobertura, passos da marcha).

## Q22: otimização geral (plano 0.43, lote 7, 2026-11-18)
Escopo: malhas, culling, compressão e o custo dos passes (o Q20 ficou com GI/denoiser, o Q21 com a visão distante).
Medidas: RTX 3060, 2560×1417, Épico, escondido, **mínimo da GPU** (o quadro mais barato) em pares alternados base `3e3ce6d`
× branch. A GPU esteve quase o tempo todo dividida com as outras sessões do lote (e um Blender aberto): as médias e as
contagens de travada dessa tarde não valem; o mínimo da GPU e os pares alternados são o que se repete.

### Medida de partida (o que mais pesa, `--gpu-passes`)
| Cena | GPU mín. | O que pesa |
|---|---|---|
| `rocks` | 4,96–5,02 ms | malhas do passe principal 2,2; cache de superfície 1,1; pré-passe 0,59; denoiser 0,56; sondas 0,35 |
| `rocks_tess` (Épico 4 px) | 6,66–6,84 ms | malhas 3,4; pré-passe 0,92 (com os microtriângulos); cache 1,2; subdivisão 0,055 |
| `heavy` | 13,5 ms | malhas 9,2 (os raios de sombra com teste de alfa nos 600 mil cartões: `--no-rt` 2,9); pré-passe 3,1 |
| `glass` (`translucent`) | 5,4 ms | denoiser / filtro das sombras 2,6 (área do Q20); malhas 1,5 |
| `pause_options` | 1,3 ms | **CPU**: um quadro de 1,4–1,6 s ao abrir as Opções (128 fps medidos, meta 200) |
| `world_partition` / `pcg_stream` | 2,1 / 1,9 ms | **CPU**: ~1,5 s de quadros de 11–15 ms logo depois do Play (ver abaixo) |

### O que entrou
1. **Tesselação: `tess.vert` próprio e os cantos por triângulo** (notas-dev/tessellation.md "Shader próprio"). O passe 9
   saiu do `mesh.vert` (61 registradores, 22,9 KB; o `tess.vert` 68, 13 KB); o microvértice lê 128 B do seu triângulo
   em vez de 8 leituras encadeadas e uma textura por canto.
   - `rocks_tess`: 6,49–6,59 × 6,61–6,64 ms (3 pares): **−0,07 ms**. A subdivisão ficou +0,03 ms (grava os cantos). O
     ganho é pequeno porque o custo do passe é a rasterização de triângulos de 4 px e o sombreamento por quad (o mesmo
     pixel sombreado por 2-3 triângulos vizinhos), não o vertex shader.
   - `rocks` (sem tesselação): 4,86–4,92 × 4,92–4,98 ms (3 pares; o passe principal 3,48–3,49 × 3,50–3,54): **−0,05 ms**,
     o que o Q17 tinha posto no `mesh.vert`.
2. **Opções do menu de pausa sem travada**: o combo "Upscaler" perguntava a cada fornecedor se roda (carrega
   `nvngx_dlss.dll`, inicia o NGX, `amd_fidelityfx_vk.dll`, `libxess.dll`) no quadro que desenhava as Opções pela 1ª vez:
   1,36 s. `gus_warm()` faz a pergunta ao começar o Play (editor) e na carga do jogo empacotado, quando o projeto tem
   menu de pausa (2,2 s de carga a mais, uma vez). `pause_options`: pior quadro 1336 → 51 ms; 294 × 199 fps (par
   alternado, sem validação).
3. **`mat_update` só diz "mudou" quando mudou**: devolvia `true` em todo quadro com material pendente, também quando
   nada foi assado (as imagens ainda decodificando, a fatia do quadro gasta). Cada `true` reenvia todas as instâncias,
   espera a GPU (`vkDeviceWaitIdle`) para mandar a info das malhas e reconstrói o TLAS. Agora: `true` se um material ou
   instância foi assado, ou o slot de uma instância mudou (`M.resend`: instância dinâmica criada / fim do Play). Os
   quadros dos flipbooks e os valores das instâncias dinâmicas já são escritos no lugar.
4. **As medidas esperam os materiais do projeto**: no editor, o Play começa enquanto todo `.rmat` do projeto é assado em
   segundo plano (37 no projeto dos testes, 4 a 211 ms cada um num quadro: o `M_HeavyRock` 211 ms), fora do que a cena
   usa. O `--bench` agora espera isso como já esperava as pipelines (`mat_compiling()`, no máximo 8 s), e as contagens
   de travada do `world_partition` e do `pcg_stream` começam depois. No jogo empacotado tudo é assado na carga.
5. `--shader-stats` imprime também o vertex shader do `mesh` e do `mesh (tess)`.
6. `REALENGINE_PCG_DENSITY` (medida): a densidade da floresta do `pcg_stream` (0,04 = a densa do exemplo).

### Medidos e descartados
- **Rolagem dos níveis das sombras virtuais** (o "Falta" do pcg.md: a câmera a 80 m/s redesenhava níveis inteiros):
  feita e medida no mesmo executável, rolagem × `REALENGINE_VSM_NO_SCROLL` — os níveis inteiros por quadro caíram de
  0,59 para 0,22, mas o passe das sombras **dobrou** em média (cópias de 2 × 16 MB por rolagem: 0,14 → 0,22 ms no
  `world_partition`, 0,02 → 0,06 no `pcg_stream`) e os quadros acima do dobro da mediana **não** caíram (floresta densa
  2,6–3,0% × 2,1–2,2%). A travada desses testes não vinha mais da sombra (o trace mostrou a CPU no `render_scene`: o item
  3). Retirada; o `vsm.c` ficou como estava.
- **Quantização de vértices e índices**: o `Vertex` tem 32 B (posição em float para a BLAS, normal octaédrica, UV, UV2).
  Nas cenas medidas o vértice não é o gargalo: na `rocks` o pré-passe inteiro (só profundidade, todos os vértices do
  corte) custa 0,59 ms; na `heavy` o passe de malhas cai de 9,2 para 2,9 ms só tirando os raios. Mudar o layout mexe no
  skin, na BLAS/CLAS, no `rt.glsl` (Q20) e nos caches em disco para ganhar menos de 0,1 ms: não feito.
- O que o resto deste arquivo lista para 200/100 fps (raios fora do fragment shader, denoiser com memória compartilhada,
  volumes, reflexo a meia resolução) é da GI, do denoiser e do céu: ficou para o Q20/Q21.

### Testes
Limites mantidos (nenhum baixado). Folga com a GPU livre (escondido, sem validação, mínimo da GPU): `translucent` 166
fps (meta 150), `rain_clear_drops` 212 (200), `eyes` 255 (200), `tess_rocks` 116–139 com a GPU dividida (110).
`pause_options` deixa de falhar pelo quadro de 1,4 s. `world_partition` e `pcg_stream` sem a GPU livre nesta tarde
continuam ~10% acima do dobro da mediana nos dois executáveis (base igual): repetir na suíte completa com a GPU livre.
