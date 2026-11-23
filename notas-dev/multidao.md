# Multidões (Crowd Spawner)

Código: `src/crowd.c/.h`, `shaders/crowd.comp`, componente `CCrowd` (`components.h`), seção "Crowd" no Details
(`editor.c`, `crowd_details`), Adicionar > IA > Gerador de Multidão. Cena de teste: `--scene crowdN` (N agentes).

## Como funciona
- **Agentes não são atores**: arrays SoA por spawner (`CrowdRun`: x, y, vx, vy, yaw, fases, papel...), passo em paralelo
  (`re_parallel_for`, blocos de 512), posições novas em `nx/ny` e troca de ponteiros (sem corrida entre leitura e escrita).
- **Campo de fluxo**: BFS de 4 vizinhos na grade de navegação da IA (`ai_nav_info`, degrau máximo 45 cm) a partir de
  onde o jogador **vai estar** (`prediction` s à frente); direção pelo gradiente de 8 vizinhos sem cortar quinas. Roda
  numa thread própria com buffer duplo (o jogo nunca espera); refeito quando o alvo muda de célula.
- **Mentes**: Hive (um vê → todos sabem; as vagas do cerco repartidas por ângulo) e Individual (cada um vê, lembra
  `memory` s, reage após `reaction` s, 64 raios de visão por quadro no total).
- **Papéis**: perseguir, cercar (anel de vagas em camadas concêntricas, uma separação entre elas, quantas o número de
  agentes pedir), vagar, fugir. `aggression` = parcela que ataca (os mais próximos, por faixas de distância).
- **Separação**: hash espacial (células do tamanho da separação), no máximo 32 vizinhos examinados; com mais de 6
  colados o agente deixa de empurrar para a frente (pressão da multidão: quem está atrás espera, como numa multidão de
  verdade) — isso tirou o O(n²) quando milhares convergem para o mesmo ponto.
- **Desenho**: as partes do Skeletal Mesh assadas por quadro (idle/walk/run, 32 quadros por ciclo); `crowd.comp` escreve
  as partes de cada agente nas instâncias da cena (depois das do mundo), misturando dois clipes; daí em diante o
  culling/LOD/sombras da GPU tratam como qualquer instância. LOD 0: tudo com sombra; LOD 1: sem sombra; LOD 2: só as 6
  partes maiores. Distâncias 15 m / 30 m, **com orçamento**: no máximo 1500 agentes em LOD 0 e 5000 até LOD 1 (um
  histograma de distâncias, 50 cm por faixa, sem ordenar).

## Medidas (RTX 3060, 2560x1417, MSAA 4x)
| Cena | FPS | Simulação (CPU) | GPU main | Partes desenhadas |
|---|---|---|---|---|
| 2000 agentes | 261-263 | 0,42-0,59 ms (passo 0,31) | 2,7 ms | 35 mil |
| 20 000 agentes, antes | 75 | 4,65 ms (passo 3,9) | 10,7 ms | 532 mil |
| 20 000, separação limitada | 115 | 1,21 ms (passo 0,70) | 6,9 ms | 386 mil |
| 20 000, + orçamento de LOD | 121-130 | 1,23 ms | 5,8 ms | 217 mil |

O campo de fluxo (1,3-2,2 ms) roda na thread própria, fora do quadro. O resto do custo dos 20 000 é a GPU rasterizando
~6 M triângulos pequenos com MSAA 4x; o próximo passo seria um impostor (um quad) para os agentes distantes.

## Testes
- `crowd`: 2000 agentes, validação ligada, > 200 fps, chegam a 3/4 da distância inicial.
- `crowd_20000`: > 100 fps, aproximam-se (o anel de 20 000 tem dezenas de camadas: a média fica longe).

## Movimento da multidão no TSR (2026-11-07, relato do usuário: borrões e rastros)
**Sintoma:** os agentes deixavam rastros e borrões com o TSR ligado. O passe de velocidade só desenhava os atores que a
CPU sabe que se moveram (`velocity_upload`). As partes da multidão são escritas na GPU pelo `crowd.comp` e ficavam de
fora, então o TSR as tratava como paradas (só o movimento da câmera) e reprojetava o histórico de onde elas estavam.
- **`crowd.comp`:** antes de trocar as linhas de cada parte, guarda as do quadro anterior em `crowd_prev` (48 B por
  parte, a partir de `crowd_base`). Um slot recém-criado (linhas zeradas) usa as atuais, ou seja, sem movimento.
- **`velocity_crowd.vert`:** o passe de velocidade desenha de novo as listas visíveis do cull (iniciais e, com oclusão,
  tardias), pelos mesmos draws indiretos do passe principal. Só as partes de multidão (`INST_CROWD`) saem; as outras
  colapsam num ponto. O movimento próprio é calculado como no `velocity.vert`, e o TSR soma o da câmera.
- **Testado e descartado:** pular os agentes distantes (LOD 1–2). Os rastros voltavam neles.
- **Medido** (RTX 3060, 1440p, mínimo da GPU, rodadas com a GPU livre):
  - **Imagem:** os agentes ficam definidos, como na referência sem TSR, em vez de riscos (comparação em zoom).
  - **Custo, 20 000 agentes:** +1,4 ms (4,42 → 5,86 ms; 187 → 147 fps no teste `crowd_20000`).
  - **Custo, 2000 agentes:** 3,25 → 3,17 ms (no ruído).
  - **Cintilação:** a medida sobe de 0,13% para 0,25% porque o borrão escondia o movimento real.
  - `REALENGINE_NO_CROWD_VEL=1` compara.
- **Próximo:** escrever a velocidade da multidão já no pré-passe, para não redesenhar as partes (o +1,4 ms).

## Cidade viva: pânico com orçamento, anéis de simulação, LOD 3 (Q27.1, 2026-11-23)
Do GTA 6 (notas-dev/auditoria-gta6.md): um tiro numa multidão de aeroporto acordava a IA cara de dezenas de NPCs no
mesmo quadro, e o jogo caía de 30 para ~20 fps e ficava lá. Aqui o custo de um pânico é espalhado por construção.
- **Crowd Alarm** (`crowd_alarm`, nó Crowd Alarm: Location, Radius): uma onda sai do ponto a 15 m/s (o tiro ouvido, os
  outros vistos correndo) até o raio. Quem a frente passou entra numa fila e é promovido a pânico, no máximo 64 por
  quadro somando todas as multidões, os mais perto primeiro (a ordem da frente). Em pânico o agente foge do ponto em
  corrida, com um desvio próprio (espalham, não saem em fila), por 6 a 12 s, e volta a andar.
- **Anéis de simulação**: o agente calmo desenhado em LOD 2 ou 3 no quadro anterior (além de ~30 m) anda em quadros
  alternados com o dobro do dt (metade deles a cada quadro). Em pânico, sempre todo quadro.
- **LOD 3** além de 60 m: só as 2 maiores partes (o `big` ganhou o bit 2; `crowd.comp` corta o resto).
- **Medidas** (RTX 3060, 1440p): `crowd_20000` passo 1,04 → 0,71 ms, simulação 1,42 → 1,14 ms, 120 → 125 fps (presa
  na GPU). `--scene panic10000` (10 000 pedestres, densidade de cidade: raio de 120 m; o jogador no meio): 289 fps; o
  alarme de 35 m pega 809, 836 entram em pânico (os que entram no raio andando), no máximo 8 promovidos num quadro; o
  segundo depois do tiro: média 1,02x e percentil 95 1,09x da mediana de antes; simulação < 1 ms antes e depois.
- **Teste `crowd_panic`**: 10 000 pedestres, alarme em 1 s; a média do segundo seguinte ≤ 1,25x a mediana de antes e o
  percentil 95 ≤ 1,5x (um quadro isolado do sistema aparece também sem alarme: medido), ≤ 64 promovidos por quadro, 90%
  dos que estavam no raio em pânico, ≥ 200 fps.
- Próximo (Q27): pele por compute com animação em textura no anel do meio e impostor animado no longe; rotinas e
  reações por tipo de pedestre (Q27.10).

## Variedade de NPCs (Q27.9, 2026-11-23: as multidões do GTA 6 que não se repetem)
- **Looks** no Crowd Spawner (0..1, no fim do `CCrowd`; níveis antigos leem 0: todos iguais): cada agente ganha um
  visual de 32 bits pela semente (`make_looks`, um fluxo de hash próprio: os outros números do spawn não mudam): tom de
  pele (8), cor de cima (32), cor de baixo (16), corpo (16: largura 0,88..1,14 em volta do próprio eixo) e o bit de
  ligado. Looks < 1 deixa essa parte com as cores da malha.
- **Regiões das partes** pelos nomes dos ossos (no `bake`, bits 2-3 do `big`): cabeça, pescoço e mãos = pele; coluna,
  clavícula e braços = parte de cima; pelve e pernas = parte de baixo; o resto fica como está.
- **GPU**: `CrowdAgentGpu.look` (36 bytes por agente); `crowd.comp` escolhe a tinta nas paletas (constantes do shader)
  e escala as linhas x/y da parte pela largura; a tinta vai no `lightmap_st` com w = 2 (as partes da multidão não têm
  lightmap) e o `mesh.frag` multiplica a cor base por ela depois do material.
- **Medidas** (RTX 3060, 1440p, cena `npc_variety`: 200 agentes): **200 visuais distintos**, 4 bytes por agente, CPU da
  multidão 0,027 ms; GPU do quadro 2,24 ms contra 2,27 sem (`REALENGINE_CROWD_LOOKS=0`): sem custo mensurável.
- Fica para depois: peças modulares de verdade (cabeças, cabelos, roupas como malhas trocáveis no mesmo esqueleto),
  morphs de proporção na pele, retarget em tempo real entre esqueletos para um conjunto de clipes servir corpos diferentes.
