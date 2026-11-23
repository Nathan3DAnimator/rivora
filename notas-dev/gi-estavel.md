# GI e reflexos estáveis (Q20, 2026-11-18)

Pedido (plano 0.43, lote 7): pesquisa do estado da arte em 2026 (ReSTIR GI/PT, radiance cache neural, MegaLights,
denoisers), escolher o que falta e implementar; qualidade sem cintilação, ghosting nem borrão na GI e nos reflexos, nos
presets Baixo a Épico, sem perder fps; metas medidas em testes.

## Pesquisa: o estado da arte contra o que a engine tem
| Técnica (2026) | O que é | Na engine | Decisão |
|---|---|---|---|
| ReSTIR DI (Bitterli 2020) | reamostragem das luzes, reuso temporal e espacial | temporal no Fotorrealista (A9.1a, [luz.md](luz.md)) | o espacial continua em aberto (ganho pequeno medido no temporal) |
| ReSTIR GI (Ouyang 2021) | reuso das amostras de ricochete | espacial no Fotorrealista Alto+ ([gi.md](gi.md)); o temporal fervia por cima do histórico do denoiser | fica como está |
| ReSTIR PT / Area ReSTIR (Lin 2022, 2024) | reuso de caminhos inteiros com shift mappings | não | fora: custo de vários ms a 1440p numa 3060, e o Fotorrealista já está em ~140 fps na casa; a referência é o path tracer |
| Radiance cache no mundo (Lumen surface cache, SHaRC do RTXGI 2) | luz guardada nas superfícies ou numa hash espacial | cache de superfície com média corrida + sondas em duas cascatas (A2) | é o mesmo papel; já estável (cintilação 0,05% parado) |
| Neural Radiance Cache (Müller 2021; NRC SDK 2025) | MLP treinado por quadro que guarda a radiância | não | fora (A9.1c segue em aberto): exige VK_NV_cooperative_vector/tensor cores (só NVIDIA), treina com ruído por quadro (instabilidade temporal, o contrário deste pedido); o cache de superfície faz o papel |
| MegaLights (UE 5.5-5.7) | luz direta estocástica com poucos raios e denoise próprio | amostragem estocástica (fase 2) + ReSTIR DI temporal + denoiser unificado | igual em espírito |
| ReBLUR / ReLAX (NRD) | pré-borrão, acumulação com rejeição de desoclusão, **history fix**, anti-lag, pós-borrão, estabilização, movimento virtual do especular | tudo, menos o history fix e o movimento virtual | **history fix entrou**; movimento virtual em aberto |
| DLSS Ray Reconstruction (DLSS 3.5/4) | rede que troca denoiser + upscaler | DLSS SR pelo SDK (upscaling.md) | em aberto: só NVIDIA, outro SDK (nvngx_dlssd) |
| TSR da UE 5 | rejeição do histórico por profundidade/velocidade nas desoclusões | o TSR só cortava o histórico em movimento de câmera; parado, a caixa larga (2,5σ) deixava passar a cor de quem saiu | **entrou** |

Conclusão da pesquisa: a parte "no mundo" (cache, sondas, lightmaps) já é estável; os rastros vêm do que guarda história
**por pixel**: o TSR e o denoiser. Foi onde a medida mostrou o problema (abaixo), e foi onde mexi.

## As medidas novas (`--bench`)
- **Cintilação parada** (já existia): mudança média da luminância entre quadros na sonda 64 x 32 (`--max-flicker`).
- **Cintilação com a câmera andando devagar**: `--cam-move x,y,z` (cm/s) e a parte da vista que **vai e volta** mais de 2%
  em dois quadros (o que desliza não conta). Limite novo: `--max-pulses <%>`.
- **Ghosting** (novo, `exposure.comp` passe 3, `render_ghosting`): sonda de 128 x 64 pontos; um ponto coberto por um ator
  em movimento (a velocidade dele no passe de velocidade, a até 3 px do render) fica negativo. Na CPU cada ponto conta os
  quadros desde que o ator saiu; a mudança média relativa (como a da cintilação) dos pontos deixados há **2 a 8 quadros** é
  o rastro; a dos pontos que nenhum ator tocou há 255 quadros é o fundo. O `--bench` imprime
  `Ghosting: X% behind moving actors (N points a frame), background Y%`; `--max-ghost <%>` falha acima, e também se
  nenhum ator em movimento apareceu (menos de 4 pontos por quadro). Só roda nas medidas (a sonda da cintilação ligada).
- **Cena `house_ghost`**: a casa com um pilar de 25 x 25 x 160 cm (Movable) indo e voltando a 2 m/s a ~3 m da câmera, na
  frente da porta acesa (`editor_ghost_tick`). O `house_walk` não servia: o personagem anda fora da vista.
- `tools/stability_bench.ps1`: os 4 presets x Realista/Fotorrealista x parado/câmera andando/pilar, com fps e GPU mínima.

## O que entrou
1. **TSR: descartar o histórico descoberto** (`tsr.comp`, `render.c`). Onde um ator anda, o ponto do cenário que ele
   descobre não tem história própria; o TSR em repouso usava a caixa de cor larga (2,5σ) e peso de 1-2,5% para o novo, e a
   cor de quem saiu ficava dezenas de quadros. Agora, nos quadros em que um ator se moveu (`post_vel`), a profundidade do
   render é copiada depois do TSR para `R.tsr_dep` (binding 85, D32 no tamanho da saída, só a região do render); no quadro
   seguinte o TSR lê os 4 texels em volta de onde o ponto estava: se todos estavam mais perto em mais de 3% (Z invertido),
   algo estava na frente dele e saiu: o histórico é descartado (`fresh`) e o pixel começa da média 3x3 das amostras deste
   quadro (75%: o ruído por quadro do GTAO e das sombras estocásticas, que o histórico mediaria, cai a um terço). Pixels
   com velocidade própria (o ator) seguem como antes. Bit 1 do `pc.pass` do TSR (o bit 0 é a paridade).
   Project Settings > Rendering > **Reject Uncovered History**, `r.TSR.Disocclusion`; `REALENGINE_NO_TSR_DISOCCLUSION=1`.
2. **Denoiser: history fix** (`denoise.comp` passe 2, como o do ReBLUR). Um pixel com histórico curto (difuso < 8 quadros,
   reflexo < 4: recém-descoberto) pesa os vizinhos na mesma superfície pela razão entre os quadros deles e os seus
   (0,25 a 16x; reflexo até 8x), e o raio do pós-borrão do difuso cresce para 28/20/14 px (histórico < 2/4/8): a faixa que um
   ator a ~5 px por quadro descobre em 8 quadros é mais larga que os 14 px de antes. Antes ele mostrava 1-2 quadros de um
   raio por pixel (fervia ao longo do rastro). Bit 11 do `pc.pass` do denoiser desliga. Project Settings > Rendering >
   **Denoiser History Fix**, `r.Denoiser.HistoryFix`; `REALENGINE_NO_HISTORY_FIX=1`.

## Medidas (RTX 3060, 2560 x 1417, `--bench 3`, janelas no 2º monitor; outra sessão usava a GPU em parte das rodadas)
Base = main `3e3ce6d` com a sonda nova; depois = este trabalho. Cintilação e vai-e-volta em % da vista; ghosting e fundo
em % de mudança média.

**Base, todos os presets** (GPU livre, uma rodada cada; `tools/stability_bench.ps1`):
| Estilo / preset | fps parado | cintilação parada | vai-e-volta, câmera andando | ghosting (fundo) |
|---|---|---|---|---|
| Realista Baixo | 663 | 3,35 | 1,72 | 1,17 (2,02) |
| Realista Médio | 466 | 0,053 | 0,18 | 0,51 (0,18) |
| Realista Alto | 345 | 0,056 | 0,17 | 0,69 (0,24) |
| Realista Épico | 270 | 0,053 | 0,17 | 0,82 (0,30) |
| Fotorrealista Médio | 255 | 0,145 | 0,23 | 2,87 (0,39) |
| Fotorrealista Alto | 166 | 0,126 | 0,30 | 2,58 (0,46) |
| Fotorrealista Épico | 144 | 0,132 | 0,36 | 2,76 (0,56) |

**Antes × depois, ghosting em house_ghost** (A/B alternado com o mesmo executável, "antes" =
`REALENGINE_NO_TSR_DISOCCLUSION=1 REALENGINE_NO_HISTORY_FIX=1`; duas rodadas cada; outra sessão usava a GPU em parte
delas, por isso o fps varia, mas cada par roda nas mesmas condições):
| Estilo / preset | antes | depois |
|---|---|---|
| Realista Médio | 0,788 / 0,790 | **0,660 / 0,626** |
| Realista Alto | 0,866 | **0,755 / 0,752** |
| Realista Épico | 0,757 / 0,720 | **0,609 / 0,634** |
| Fotorrealista Médio | 3,999 / 3,846 | **1,820 / 1,854** |
| Fotorrealista Alto | 3,054 / 2,978 | **1,672 / 1,611** |
| Fotorrealista Épico | 2,978 / 2,923 | **1,635 / 1,584** |
Com a GPU livre (antes, mesmas condições): Realista Épico 0,745-0,81 → 0,62-0,68; Fotorrealista Épico 2,58-2,78 →
1,36-1,57. A cintilação parada e o vai-e-volta com a câmera andando não mudam (A/B: Realista Épico 0,35-0,41 x 0,41;
Fotorrealista Épico segue o fps da rodada, 1,85-2,62 x 1,89-2,72, com a GPU dividida): nenhum dos dois age sem ator em
movimento, fora pixels de histórico curto.

- **Por que o Baixo é alto em tudo:** a casa no Baixo não tem GI (quase preta, ~0,03 nits): a variação relativa é ruído do
  escuro, como já registrado em [luz.md](luz.md). Nenhum teste usa o Baixo da casa.
- **O ghosting depende do fps**: com a GPU dividida o pilar anda mais por quadro e o número sobe (Realista Alto 0,67 com a
  GPU livre, 0,87 dividida, na mesma build). Compare sempre em A/B.
- **Custo** (mínimo de 4 rodadas alternadas, `--gpu-passes`, house_ghost Épico): TSR + cópia 0,588 → 0,621 ms (+0,03, só
  com um ator em movimento: parado a cópia não roda e o teste não lê nada); pós-borrão do denoiser 1,067 → 1,130 ms
  (+0,06; parado quase nada, o fix só age onde o histórico é curto). GPU mínima por quadro igual dentro do ruído (Realista
  3,14 × 3,19 ms; Fotorrealista 6,49 × 6,47 ms). CPU igual (nada novo fora das medidas).
- **Cintilação parada** igual (Realista Épico 0,053%, Fotorrealista Épico 0,13%); o vai-e-volta com a câmera andando
  também.

### Testes (área `gi`)
| Teste | Configuração | Mediu (3 rodadas, validação) | Limite |
|---|---|---|---|
| `ghost_realistic` | house_ghost, Realista Épico | 0,626-0,658% (antes 0,75-0,81) | `--max-ghost 0.8`, 150 fps |
| `ghost_medium` | house_ghost, Realista Médio | 0,507-0,541% | `--max-ghost 0.7`, 250 fps |
| `ghost_photoreal` | house_ghost, Fotorrealista Épico | 1,36-1,52% (antes 2,58-2,78) | `--max-ghost 1.9`, 80 fps |
| `stable_pan` | house, câmera a 28 cm/s, Realista Épico | vai-e-volta 0,17-0,18% | `--max-pulses 0.25`, 200 fps |
| `stable_pan_photoreal` | idem, Fotorrealista Épico | 0,35-0,36% | `--max-pulses 0.45`, 90 fps |
A cintilação parada segue nos testes de antes (`gi_house` 0,12%, `denoiser_house` 0,2%). O ghosting e o vai-e-volta
sobem com a GPU dividida (o pilar anda mais por quadro): com outra engine aberta eles falham junto com o fps; repita com a
GPU livre (como os outros testes de fps desta suíte).

## Falta
- Movimento virtual do especular no denoiser (reprojetar o reflexo pela distância do acerto, ReBLUR): um reflexo nítido
  ainda escorrega alguns quadros com a câmera andando; precisa guardar a distância do acerto especular (o byte alfa livre de
  `o_guide.x`).
- O que resta do rastro no Realista é o ruído por quadro dos pixels recém-descobertos (GTAO girando, sombras estocásticas):
  um TSR com anti-aliasing espacial para pixels novos (como o da UE) o reduziria mais.
- ReSTIR DI espacial, DLSS Ray Reconstruction, NRC (A9.1c), A2 (quadros por face, atlas virtual, relocação das sondas,
  coleta a meia resolução).

## Merge do lote 7 (18/11)
- A sonda de ghosting (8192 pontos) era lida pela CPU a cada quadro de memória HOST_COHERENT, sem cache no NVIDIA: +0,8 ms
  de CPU no render em todo `--bench` (`demo_fighting` 0,9 → 1,7-2,2 ms). Os buffers lidos pela CPU (`exposure_read`,
  `flicker`, `ghost`) agora usam memória HOST_CACHED (`host_memory_type` no render.c): 0,74-0,84 ms, igual à base.
- `depth_resolve` ganhou TRANSFER_SRC: a cópia da profundidade para o TSR acusava VUID com MSAA (`project_quality`).
