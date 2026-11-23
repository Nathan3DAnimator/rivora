# Tecido (2026-11-08, pedido do usuário)

Pedido: "simulação de tecidos avançada, o melhor do mercado (NVIDIA, o OGC...), o melhor algoritmo de tempo real, de
melhor qualidade e desempenho, uma aba própria; múltiplos tecidos pesados ao mesmo tempo, interagindo com objetos e
Skeletal Meshes sem quedas de fps, e podendo rasgar".

## A escolha
- **Vertex Block Descent** (Chen, Macklin, Kim, Yuksel, SIGGRAPH 2024): um passo de Newton por vértice (inércia + a
  energia das molas dele, Hessianas projetadas para serem positivas), os vértices de uma cor em paralelo. Estável com
  qualquer rigidez e passo (o XPBD/NvCloth precisa de muitas iterações para tecido rígido), converge em poucas passadas.
- **Contato como no Offset Geometric Contact** (Chen et al., SIGGRAPH 2025, "OGC"): o deslocamento de um vértice num
  passo é limitado a 0,45 da distância até o colisor mais perto: nunca atravessa; depois é empurrado para a superfície
  deslocada pela Espessura, com atrito. (Contato vértice-colisor; o OGC completo também faz aresta-aresta e
  auto-colisão: não feito.)

## Arquivos
- `src/cloth.h/.c`: buffers, presets, colisores, listas por cor, passadas, desenho, `cloth_tear`. `CCloth`
  (`COMP_CLOTH`): tamanho, resolução, tipo, Preso, densidade, rigidez (esticar, cisalhar, dobrar), amortecimento,
  atrito, espessura, vento/rajada, gravidade, rasgo, cor/aspereza/sheen/translucidez, iterações/subpassos, Seguir
  Ator/Osso.
- `shaders/cloth.glsl` (dados), `cloth_sim.comp` (passadas: 0 previsão — gravidade, vento na face, pinos seguem o ator;
  1 uma cor; 2 velocidades e rasgos; 3 corte; 4 repouso), `cloth.vert/.frag` (normais da grade, buracos onde rasgou, dois
  lados, sheen Charlie, translúcido por trás, sombra do sol).
- Editor: Adicionar Rápido > Tecido > tipo; Detalhes (seção Tecido); **Janela > Simulação de Tecido** (todos, custo,
  Recomeçar Todos, Rasgar o Selecionado). Nós: **Tear Cloth**, e para a chuva Set/Get Rain Intensity.

## Como funciona
- Grade regular no plano XZ do ator; as molas saem da grade (sem lista de adjacência): 4 de esticar, 4 de cisalhar, 4
  de dobrar (distância 2). 9 cores: (x mod 3, y mod 3); listas de vértices por cor feitas na CPU quando o layout muda
  (cada passada de cor só lança os seus: 9× menos threads).
- Colisores por tecido (até 96): ossos das Skeletal Meshes (cápsula até o osso pai, raio do Physics Asset ou do
  comprimento), cápsulas de personagens, esferas e caixas das malhas do nível (planos viram lajes de 1 m: o que passa
  por baixo de um plano fino caía para sempre).
- Rasgo: mola de esticar/cisalhar com deformação acima de Rasga ao Esticar marca o bit na máscara do vértice (as de
  dobrar junto); o quad com uma aresta rasgada some no desenho.

## Sombra (2026-11-08, "ajuste o que falta")
- **Projetar Sombra** por tecido (`CCloth.cast_shadow`, padrão ligado; o componente cresceu no fim).
- **Sombras virtuais do sol**: `cloth.vert` com `pc.pad & 0x10000` desenha a profundidade no nível (a matriz e o
  retângulo do job, como a passada 1 do `mesh.vert`), junto dos lançadores dinâmicos (`VSM_PASS` modo 2, pipeline
  `R.cloth_shadow`). Enquanto simula, a caixa onde o tecido pode estar (o ponto de cima ± a diagonal) e a do quadro
  anterior viram retângulos sujos (`vsm_dirty_box`): parado, a sombra fica no cache.
- **Sombra por raio** (o híbrido usa raio nos bloqueadores perto: sem isso a sombra do pano encostado sumia): um BLAS
  por tecido (`GiDynBlas` com passo de 16 bytes, os vértices em metros, a instância com escala 100) na **metade da
  grade** (um quad por bloco 2 × 2, a passada 5 do `cloth_sim.comp` escreve os triângulos; bloco rasgado: degenerado).
  **Refit** a cada quadro; montagem completa só quando os índices mudam (Rasga ao Esticar ligado, corte por Blueprint,
  layout novo) e a cada 64. Entra no TLAS dinâmico com máscara 0x02 (só raios de sombra: nenhum raio sombreia um acerto
  no pano) e amplia a caixa do conjunto dinâmico (`cloth_bounds`).
- Medir sem: `REALENGINE_NO_CLOTH_SHADOW=1`.

## Auto-colisão (2026-11-08)
- **Auto-Colisão** por tecido (padrão ligada) e **Distância da Auto-Colisão** (cm; auto = o maior entre 2 × Espessura e
  3/4 do passo da grade: o meio de um quad fica ao alcance dos cantos, uma camada não escorrega entre os vértices de
  outra). Vale contra ele mesmo e contra os outros tecidos que a têm.
- Vértice contra vértice (como o NvCloth): a cada subpasso, depois da previsão, um hash espacial na GPU (65.536 baldes,
  célula = 2 × o maior raio; passada 6 esvazia, 7 insere por `atomicExchange` numa lista ligada), e a passada 8 acha os
  4 contatos mais perto de cada vértice (de outro tecido, ou do próprio a mais de 2 passos de grade: as molas cuidam dos
  vizinhos), no alcance dos raios somados + um raio de folga. No passo de Newton do VBD cada contato mais perto que os
  raios vira uma mola dura só de compressão (k = a rigidez de esticar, ≥ 1000 N/m): força e Hessiana `n nᵀ`.
- Limites: sem aresta-aresta nem vértice-triângulo (o OGC completo); movimento muito rápido entre camadas (mais que a
  folga num subpasso) ainda pode atravessar: mais subpassos resolvem.

## A GPU caía (device lost) na cena de tecido (2026-11-08)
Diagnóstico do assistente do usuário (Jarvis), em [diagnostico-gpu-tecido.md](diagnostico-gpu-tecido.md): o evento 153
do nvlddmkm (reset da GPU) várias vezes; a cena `cloth` perdia o dispositivo em 4 de 5 execuções (o commit da neve
também, então não era da sombra). Causa: `cloth_fill` reescrevia a cada quadro buffers mapeados únicos (header, info,
colisores, dono, ordem) enquanto a GPU ainda rodava o quadro anterior; o `memset` em `info[n]` deixava `nx = 0` por um
instante (`l % c.nx`, leitura fora do buffer). Correção:
- um conjunto por quadro em voo (`RE_FRAMES_IN_FLIGHT`) dos buffers que a CPU escreve; `cloth_fill(w, dt, slot)`;
- `ClothInfo` montado na memória comum (`C.cinfo`) e copiado inteiro; a ordem por cor montada uma vez e copiada para
  cada conjunto que ainda não a tem (versão);
- proteções no shader: `ci < ncloth`, `nx, ny >= 2`, o vértice dentro do bloco (`cloth_sim.comp`, `cloth.vert`).
Depois: 0 falhas em 6 + 14 testes seguidos. O TDR por carga (4 subpassos × 24 iterações × 9 cores) continua possível
num PC fraco com muitos tecidos rígidos: o padrão é 1 × 6.

## Medidas (RTX 3060, 1440p, `--scene cloth`: 12 tecidos, 34.560 vértices)
| Variante | GPU mínima | Custo |
|---|---|---|
| sem tecido | 1,45 ms | — |
| 2 subpassos × 8 iterações, todas as threads por cor | 2,70 ms | 1,25 ms |
| ... com listas por cor | 2,42 ms | 0,97 ms |
| 1 subpasso × 6 iterações (padrão) | 1,84 ms | 0,39 ms |
O custo é quase todo o número de passadas (57 a 1×6), não os vértices: mais tecidos custam pouco a mais.

Auto-colisão (os 12 tecidos, com sombra): 2,25 → 2,46 ms (+0,21: 3 passadas por subpasso e a busca em 27 células).

Sombra (os 12 tecidos): 1,90 → 2,25 ms (+0,35: sombras virtuais 0,16, raio 0,19). Montando os 12 BLAS inteiros a cada
quadro era +0,32 só disso; com refit e meia grade, ~0,1.

## Bugs achados
- O amortecimento do VBD (k_d / h) estava ~1000× forte: congelava os tecidos. Agora Damping 0,02 = k_d 2e-5 s.
- Plano como caixa de espessura zero: um vértice que passava por baixo era empurrado para baixo até virar NaN.

## Malha própria: a roupa importada (2026-11-08)
- `CCloth.mesh` (o id de uma malha importada; 0: a grade), `mesh_scale`, `pin_band`/`pin_axis` (presos os vértices a
  essa distância do topo ao longo do eixo), `follow_turn` (seguindo um osso, o giro inteiro: a malha de uma parte está no
  espaço do osso; 0: só o yaw, como as capas).
- **Grafo** (`cloth_mesh.c`, na CPU uma vez por malha e ajuste, 0,3-0,4 ms para 816 vértices): os cantos soldados pela
  posição (0,01 cm: a costura de UV não vira rasgo), triângulos, molas de estiramento nas arestas, de dobra entre os dois
  vértices opostos de cada aresta interna, os triângulos de cada vértice (a normal), até 48 entradas por vértice;
  **cores** gulosas (o mais ligado primeiro, até 32): a saia de 816 vértices tem 7, o tubo do autoteste 4. As listas de
  cores do cabeçalho cresceram de 9 para 32 (lidas do buffer: uma cópia privada indexada por variável ia para a memória,
  a cena de grade custava +0,24 ms).
- **GPU**: os mesmos passes e buffers da grade com `CLF_MESH` (nx = vértices, ny = 1) e os buffers `m*` do cabeçalho
  (repouso com o preso em w, UV, adjacência, triângulos; escritos só na troca de layout, com a GPU parada; as normais
  pelo passe 9 novo, só quando há malha). Molas pela adjacência no passe 1; vértice cortado (Tear Cloth) sem molas e sem
  triângulos; rasgo por estiramento só na grade. BLAS com os triângulos da malha (2 por vértice no espaço do `idx`).
- **Amortecimento relativo** nas molas da malha: o do artigo do VBD (o Hessiano vezes o passo inteiro do vértice) também
  freava a translação: a saia levada pelo quadril ficava 24 cm atrás numa caminhada; com o passo relativo das duas
  pontas, 16 cm (o resto é o ar). A grade continua com o do artigo.
- **Ar numa roupa fechada**: a malha tem normais para fora (como os importadores); a pressão só na face que o ar atinge,
  o lado de trás com 15% (sucção): antes as costas da saia eram empurradas para fora como uma bandeira.
- **Colisores das partes**: as partes de uma Skeletal Mesh penduram no ator (anim.c), não no osso pai: a regra antiga
  (cápsula até o pai) nunca valia e as pernas não seguravam nada; agora uma cápsula ao longo do lado maior da caixa da
  malha da parte, larga como os outros. A parte que o tecido segue fica de fora (os presos ficam dentro dela).
- **Ator > Criar Tecido a partir da Seleção**: a malha importada selecionada vira um Tecido no lugar (escala, cor, eixo
  para cima no espaço da malha, faixa de 5% da altura); uma parte de personagem segue o osso com o giro inteiro; a malha
  original escondida.
- Cena `--scene cloth_mesh` (com `--new-project`): uma saia gravada como `.rmesh` (costura de UV dobrada, como sai de um
  importador) na pélvis do Walker que anda em círculo a 0,9 m/s, e outra feita pelo menu sobre um suporte.
  `REALENGINE_WALKER_STILL=1` para a foto parada; `REALENGINE_CLOTH_NO_COLLIDERS=1` mede sem colisores.
  Testes `cloth_mesh_graph` (solda, molas, cores sem mola dentro, presos) e `cloth_mesh` (2 tecidos).
- Medidas (RTX 3060, 1440p): cena `cloth_mesh`, GPU 1,65 ms sem tecido e 2,86 ms com as 2 saias (1632 vértices): o custo
  é a corrente de ~50 despachos dependentes (7 cores × 6 iterações + os outros), como os 12 tecidos da grade (34 560
  vértices, 1,35 ms); o número de vértices quase não pesa. Cena `cloth`: 2,79 → 2,80 ms (no ruído).

## Falta
- Aresta-aresta e vértice-triângulo do OGC (a auto-colisão é vértice-vértice); o tecido não entra no GI (luz rebatida).
- Malha própria: pesos pintados à mão (hoje a faixa do topo), skinning dos presos em vários ossos (hoje um osso), rasgo
  por estiramento; juntar os despachos das cores (o custo fixo de ~1 ms é a corrente de barreiras).

## Corpos moles (Q9, 2026-11-18)
O mesmo núcleo (VBD, contato OGC, auto-colisão entre corpos) simula volumes: `CCloth.kind` = Soft Body, ver
[soft-body.md](soft-body.md). Mudanças aqui: colisores com 3 vec4 (caixa orientada, tipo 4), `collide` diz qual colisor,
o `pad` do passe 1 leva 0x100 na última iteração e o do passe 0 o subpasso. Custo (lado a lado com a main, GPU livre): passe
"tecido (simulacao)" `cloth_mesh` 1,22 → 1,34 ms, `cloth` 0,70-0,74 → 0,77-0,84 ms (o colisor com 3 vec4 e o índice do mais perto);
os fps da cena no ruído (297 → 285-294).

## Roupa no corpo: colisores do Physics Asset que se movem (Q10, 2026-11-18)
- `gather_colliders` pede ao `ikrig.c` as cápsulas do Physics Asset de cada personagem perto (`phys_capsules_near`: a pose
  final deste quadro e a do anterior, publicadas por `phys_publish_bodies` no job de pose) e as manda como **tipo 4**
  (o lugar de agora) seguido do **tipo 5** (o do quadro anterior; não é um colisor sozinho). Um personagem com Physics
  Asset não manda mais as cápsulas grosseiras das partes (`REALENGINE_CLOTH_NO_BODIES=1`: o comportamento de antes, para
  medir). Fica de fora o corpo do osso que o tecido segue (os presos ficam nele).
- `cloth_sim.comp` (`collide`): o tipo 4 interpola as duas cápsulas pelo subpasso (`pc.pad` bits 8-11 o subpasso, 12-15
  quantos; a cor nos 8 de baixo) e, se o vértice está dentro agora mas estava do outro lado do eixo antes, a normal é a
  de antes e a profundidade atravessa: ele é empurrado para a frente do membro, não através dele.
- Seguir Osso acha o osso de uma malha com skin pelo `anim_socket_world` (antes só por uma parte com o nome do osso).
- Presets **Camisa** e **Saia** (`CLOTH_SHIRT`, `CLOTH_SKIRT`, no fim do enum): finos (0,6 cm), 2 subpassos × 4 iterações,
  auto-colisão, presos na faixa de cima girando com o osso; a Capa ganhou 1 cm e 2 × 4. Custo no `cloth_on_body`: o
  passe do tecido 2,28 ms de GPU (1,90 só com as partes).
- `cloth_debug_vertices(entidade)`: os vértices mapeados, só com `REALENGINE_CLOTH_READBACK=1` (o teste `cloth_on_body`).
