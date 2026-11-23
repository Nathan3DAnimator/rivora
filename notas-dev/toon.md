# Toon e Auto Adaptive Outline (Q11, 2026-11-18, plano 0.43 lote 4)

Pedido: "Shading Model Toon por material (rampa editável ou textura, número de faixas, cor da sombra, especular
estilizado, rim, hachura, luz de borda, recepção de sombra com cor), toon por luz; contorno por objeto (cor, largura, por
material) com Auto Adaptive Outline: a largura em pixels segue a distância ao longo da própria silhueta (perto fino,
longe grosso, curva editável), por profundidade+normal+ID, sem serrilhar nem cintilar com o TSR. Pronto: cena `toon`
medida (< 0,5 ms o contorno a 1440p)."

## Shading Model Toon
- **Material > Details > Modelo de Sombreamento = Toon** (`MAT_SHADING_TOON`, `"shading_model":"Toon"`). Os valores ficam
  em `MatToon` (`src/toon.h`): no `.rmat` em `"toon"` (faixas, suavidade, limite, cor da sombra, cor da sombra
  recebida, rampa de até 4 pontos ou imagem, brilho, luz de borda, hachura, ambiente) e `"outline"` (qualquer modelo).
  Uma **Material Instance** substitui tudo com **Substituir Toon e Contorno** (`"toon_own"`).
- **GPU**: um bloco de 12 vec4 por slot de material (`GpuToon`, `shaders/toon.glsl`) numa tabela depois da `GpuMatExt`
  no mesmo buffer (+768 KB). `MATF_TOON` (2048) + `MATF_EXT`: o material entra nas variantes RE_EXT; os lóbulos do
  Principled ficam desligados (`g_ext` = 0) e o toon substitui a BRDF. Espelho na CPU por slot (`material_gpu.inc`
  `toon_slot_put`): o contorno e os nós leem dali.
- **Luz** (`mesh.frag shade_light`, só nas variantes RE_EXT): a sombra fica separada do alcance (`tv`); o nível é o
  meio-Lambert (n·l/2 + 1/2) deslocado pelo **Limite da Sombra**, cortado em **N faixas** com borda suave
  (`toon_band`), o tom pela rampa (pontos ou imagem), `F = sombra + (1 − sombra) · rampa`; a sombra projetada vira uma
  faixa própria na **Cor da Sombra Recebida** (só no lado iluminado, sumindo perto do terminador: a acne de um mapa
  virava degraus). Brilho estilizado (corte em n·h com tamanho e dureza), luz de borda (borda rasante no lado que a luz
  alcança). Uma luz local de sombra sob o split dos raios (`GI_SHADOW_SPLIT`) usa o raio próprio numa superfície toon:
  a faixa precisa da visibilidade dela.
- **Indireta**: rugosidade 1, dielétrico (sem reflexo do céu; o brilho é o estilizado), fora do denoiser, vezes o
  **Ambiente do Toon** (0,6 por padrão: as faixas leem por cima). **Hachura em tela** depois da luz: linhas no espaço
  da tela sem o jitter do TSR (firmes), espaçamento em px a 1440p, cruzadas onde o tom médio das luzes (ponderado pela
  luminância de cada uma, 2 halfs num registrador) é mais escuro.
- **Toon por luz** (`CLight.toon_mode`, bits 0x40/0x80 do `GpuLight.type`; `light_kind` agora `& 7`): **Como o
  material**, **Forçar Toon** (qualquer material sob essa luz num degrau cel duro, `toon_forced`, também nas variantes
  simples, sem registrador a mais) e **Nunca Toon** (um material Toon iluminado suave por ela: luz de preenchimento).
- **Baixo / sem raios**: com mapa de sombra local ou SSR o renderer escolhia `mesh_lsm` e as variantes RE_EXT esperavam
  um quadro sem mapa (o toon sumia no Baixo; os lóbulos, a chuva e a Light Function também). Variante nova
  **`mesh_lsm_x`** (RE_LSM + RE_EXT) para quando as duas coisas valem.
- **Prévias**: `preview.frag` acende um material Toon com as três luzes do estúdio pelas mesmas funções (o Material
  Editor e todo editor de asset mostram as faixas e a hachura).

## Contorno (Auto Adaptive Outline)
- **Por material** (`MatToon.outline`: cor, largura em px a 1440p, Auto Adaptativo com Distância Perto/Longe, Escala
  Perto/Longe e Curva, Sensibilidade da Profundidade, Ângulo da Dobra) em qualquer modelo de sombreamento, ou **por
  ator** (`CStaticMesh.outline` / `outline_width` no fim do componente: Como o material / Ligado com cor e largura /
  Desligado; sem componente novo). Um Skeletal Mesh: o nó leva às partes (`anim_mesh_parts`).
- **Passe** (`src/outline.c`, `shaders/outline_id.frag`, `shaders/outline.comp`), depois da SSS/fogo, antes dos
  translúcidos e do TSR, no tamanho do render:
  1. **IDs**: os atores com contorno (um byte por slot marcado no `instance_of`; recolhidos quando um contorno liga/desliga:
     `mat_outline_generation`) num R32_UINT com profundidade própria, `mesh.vert` passe 7 (a projeção com o jitter do
     passe principal), no LOD que o cull escolhe (`vel_visible`), com o corte do Masked: entrada da tabela do quadro (8
     bits), hash do ator (8), normal octaédrica (16).
  2. **Silhuetas + x** (tiles 64×4): um pixel do ator visível (a profundidade da cena não está na frente dele por mais
     de 1%: nada escondido ganha contorno) é semente quando um vizinho é outro ator/fundo atrás dele, quando a própria
     profundidade salta além do que a inclinação prevê, ou numa dobra da normal. O raio vem da **própria distância**
     (largura × altura do render / 1440 × curva entre perto e longe): ao longo da silhueta. Cada pixel guarda a semente
     que chega mais longe nele: (r + ½)² − dx² (memória compartilhada: uma linha).
  3. **y + cor** (tiles 8×32): − dy²: o **disco exato** de cada semente com dois passes separáveis (50 leituras da
     memória compartilhada em vez de uma janela 25×25), a borda com cobertura (anti-serrilhada; abaixo de 1 px fica mais
     fraco em vez de sumir), misturado na cor antes do TSR: o jitter de cada quadro e o histórico do TSR alisam a linha
     como qualquer borda (não cintila). Tiles sem semente saem cedo.
- A cor da linha é a da tela: guardada pela inversa do ACES (o 0,6 incluído) no HDR pré-exposto.
- Medidas próprias: timestamps (mediana dos últimos 64), a fração da tela coberta pela linha (contador atômico: a
  medida de imagem do teste), Debug Viewer (F10) > **Toon Outline**; `gpu_mark` "contorno (Q11)".

## Nós de Blueprint (5, no fim do enum)
**Set Toon Parameter** / **Set Toon Color** (por nome, inglês ou português; a instância dinâmica do ator, sem bake: o
bloco do slot é escrito), **Get Toon Parameter**, **Set Actor Outline**, **Set Light Toon Mode**.

## Exemplos e teste
- `Materials/Examples/Toon`: M_Toon, M_ToonRamp (4 pontos, borda), M_ToonHatch (hachura), M_ToonSkin (faixas suaves,
  sombra recebida vermelha), M_ToonGround (sem contorno), MI_ToonBlue (instância com toon próprio).
- `--scene toon` (`src/editor_toon.c`): palco toon, os exemplos, uma esfera Default Lit com contorno vermelho do ator sob
  um spot Forçar Toon, um preenchimento Nunca Toon, 16 atores indo até 60 m. Sem nuvens (o flicker do teste é da cena).
  `REALENGINE_NO_OUTLINE` / `REALENGINE_NO_TOON`: o "antes".
- ctest (área `render`): `toon` (Épico, TSR, `--max-flicker`), `toon_msaa` (Cinemático, MSAA 8x), `toon_low` (Baixo,
  mapas): FAIL se um exemplo não for Toon, se a linha cobrir fora de 0,2–15% da tela, ou se o contorno custar ≥ 0,5 ms
  de GPU a 1440p (o tempo medido escalado pelos pixels do render até 2560×1440).

## Medidas (RTX 3060)
GPU livre (nenhum outro `realengine.exe`), 2560x1417, mailbox, Release. Cena `toon` no Épico (TSR, render 1599x992):
| | GPU do quadro | GPU mínimo (passe principal) | fps | flicker |
|---|---|---|---|---|
| Sem toon nem contorno (`REALENGINE_NO_TOON`: os mesmos atores Default Lit) | 3,199 ms | 2,871 ms (1,554) | 305 | 0,609% |
| Toon, sem contorno (`REALENGINE_NO_OUTLINE`) | 3,456 ms | 3,097 ms (1,772) | 283 | 0,436% |
| Toon + contorno | 3,58-3,61 ms | 3,23 ms (1,90) | 271-275 | 0,437% |
- **Contorno**: 0,120 ms de GPU (mediana dos timestamps; `gpu_mark` 0,127 ms), **0,28 ms escalado a 2560x1440 nativo**
  (meta < 0,5), 22 atores, 6 visuais, a linha em 0,32% da tela, CPU da lista 0,004 ms. Cinemático (MSAA 8x, render
  2132x1322): 0,197 ms (0,26 a 1440p). Baixo (render 1066x661): 0,067 ms. O flicker não muda com ele (0,436 → 0,437%):
  o TSR alisa a linha como qualquer borda.
- **Toon no passe principal**: +0,22 ms de mínimo nesta cena (as variantes RE_EXT no lugar das simples; a hachura, a
  rampa). O flicker cai (0,609 → 0,436%): as faixas não têm ruído de reflexo nem do denoiser.
- **Cenas sem toon** (A/B: a mesma build com o `mesh.frag` de antes): `house` (variantes simples) GPU mínimo 1,788 →
  1,795 ms (ruído), 479 → 476 fps; `materials` (variantes RE_EXT: lóbulos) mínimo 3,467-3,483 → 3,541-3,545 ms (passe
  principal 2,34 → 2,40: +0,06 ms, +2,6%), 248-250 → 244-247 fps.
- **Registradores** (`--shader-stats`, antes → depois): `mesh` 96/0 B, `mesh_rt` e `mesh_rt_t` 128/16 B (iguais: o
  `--no-spill` continua em 32 B); `mesh_x` 127/0; `mesh_rt_x` e `mesh_rt_t_x` 128 com 80 → 96 B de memória local; novo
  `mesh_lsm_x` 96 com 176 B (só no Baixo com material RE_EXT e mapa de sombra local ou SSR).
- Materiais: +768 KB no buffer dos materiais (a tabela GpuToon); a tabela do contorno 256 x 64 B por quadro em voo; os
  alvos do contorno 12 B por pixel do render (id, profundidade, passe x: 19 MB a 1599x992).

## Custo zero sem toon: as variantes RE_TOON (2026-11-18, depois do merge do lote 4)
- O merge do lote 4 achou a cena `rain` (variantes RE_EXT, sem material toon) mais lenta: passe de malhas 2,04 → 2,12 ms
  com o Q11 (o `tv` separado, o `toon` vivo por luz, o `toon_forced` em todo laço; `mesh_rt_x` 80 → 96 B de memória
  local). Bissecção: tirar a hachura ou o estado global (`g_toon`) não mudava nada; o caminho de luz toon dentro do laço
  das luzes (até num `return` antecipado, até em laços próprios) segurava os registradores, e só os testes do Forçar
  Toon no caminho comum ainda custavam ~0,1 ms.
- Agora **todo o código do toon fica em variantes gêmeas `RE_TOON`** das RE_EXT (`mesh_<x>_toon.frag.inc`, também as do
  denoiser e a `mesh_lsm_x`: +28 SPIR-V embutidos). O render troca para elas (`R.toon_spv`, os pipelines refeitos uma
  vez no começo do quadro seguinte) quando aparece um **material Toon** ou uma **luz Forçar Toon** (`lights_toon_force_seen`;
  com ela o passe usa as variantes RE_EXT para todos os materiais, como a chuva). Sem toon, os shaders são **os mesmos
  bytes** de antes do Q11.
- Dentro do RE_TOON a sombra não fica mais separada (`tv`): `shade_light` calcula a luz como sempre e no fim vai para
  `toon_end`, que tira a sombra de `atten` dividindo pelo alcance calculado de novo; a hachura usa o tom da luz mais
  forte (um só registrador com o slot).
- Achado de passagem: o ramo do `build_msaa_pipelines` que refaz o `mesh_x` usava o `mesh.vert` simples (a cor de vértice,
  location 5, não escrita: VUID-RuntimeSpirv-OpEntryPoint-08743 ao refazer); agora o `mesh_x.vert`.
- Medidas (GPU livre, 2560x1417, Épico): `rain` passe de malhas 2,06-2,07 ms (o mesmo build sem nada do Q11 no shader:
  2,06-2,09; a base medida pelo merge: 2,04), `mesh_rt_x` 80 B; `materials` GPU mínimo 3,50 ms (passe principal 2,35;
  a base: 3,47-3,48 / 2,34); `toon` 248-252 fps, contorno 0,119 ms (0,28 a 1440p). `rain` a 176-181 fps também sem o
  Q11 nesta máquina hoje (o `rain_clear_drops` pede 200).

## Falta
- Contorno nos atores de multidão (`INST_CROWD`), nos cabelos (Groom), no tecido/corpo mole (desenhados pelos shaders
  próprios) e nas partículas; contorno de translúcidos. O toon também não chega ao `cloth.frag` / `hair.frag`.
- O toon nas variantes Masked (cartões de cabelo/folhas toon): hoje o material Masked + Toon é desenhado pelas corridas
  masked com RE_EXT, mas sem a hachura testada.
- `mesh_lsm_x` derrama 176 B no Baixo (só com material RE_EXT e mapa de sombra local/SSR).
- Uma luz Forçar Toon não alcança as corridas Masked de materiais sem RE_EXT (elas seguem com as variantes masked comuns).
