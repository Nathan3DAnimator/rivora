# Upscaling (`upscale.c`)

Pedido (plano de melhorias, 4.4a, B15c, A10.2, R11): o vídeo-realismo a 100 fps no Épico só fecha com a resolução
interna perto de 960p a 1080p, reconstruída por um upscaler temporal. O TSR da engine já existia (`render.md`, Fase 1
da otimização); faltavam os modos como os fabricantes chamam, a resolução dinâmica, o mip bias e o lugar para FSR,
DLSS e XeSS.

## Modos (`UPM_*`)
| Modo | Render | Nota |
|---|---|---|
| Predefinição | a da escalabilidade (Baixo 50, Médio 60, Alto 67, Épico 75, Cinemático 100) | o padrão |
| Nativo | 100% | o upscaler só como antisserrilhado (DLAA / FSR Native AA) |
| Qualidade | 67% | |
| Equilibrado | 58% | |
| Desempenho | 50% | |
| Ultra Desempenho | 33% | |
- Configurações do Projeto > Rendering (Upscaler, Modo de Upscaling, Resolução Dinâmica, FPS alvo); no jogo, as opções
  `UpscalingMode` e `DynamicResolution` do Game User Settings (WBP_Options novo traz as duas linhas). A Escala de Resolução
  (50-100%) continua multiplicando por cima.
- Linha de comando (medidas): `--upscale-mode <0-5>`, `--dyn-res <fps>`, `--upscale-test`.

## Resolução dinâmica
`upscale_dynamic`: janelas de meio segundo de quadros; acima de 92% do alvo desce de uma vez (pela raiz do excesso, o
custo vai com os pixels), abaixo de 70% por 2 s sobe 5% (o `mobile_res_step` do Android). Entre metade da parte do modo
e ela. Cada passo refaz os alvos do render (uma pausa curta): por isso só quando o atraso se mantém.

## Mip bias
`tsr_mip_bias` (common.glsl): as texturas dos materiais leem com bias de log2(render / saída), preso entre -2 e 0 (a 75%,
-0,42; a 50%, -1), no `mesh.frag` (base, normal, emissiva) e no pré-passe Masked (o mapa de sombra do sol fica sem bias).
Sem TSR, 0. Cintilação conferida: gi_house 0,088%, house_walk 0,112%, demo_rpg_flicker 0,236%, demo_horror 0,062% (todos
abaixo dos limites).

### Texturas borradas a poucos metros (2026-11-23, pedido do usuário)
O chão quadriculado (M_Grid) perdia o padrão fino de pontos poucos ladrilhos à frente, no viewport e no jogo; em 100% com
TSR também, e sem TSR ia bem mais longe. Três causas, comparadas em capturas do mesmo enquadramento (`--cam`, um projeto
novo por captura: um projeto aberto de novo traz a câmera salva):
- **O núcleo do TSR**: `exp(-2.29 d²)` (o do TAA da Unreal 4, sigma 0,47 px do render) deixava ~1/3 do contraste do
  detalhe mais fino. Agora `TSR_KERNEL` 4,5 (sigma 0,33): o jitter dos quadros ainda cobre o pixel; em 100% o padrão vai
  tão longe quanto sem TSR. Cintilação (`--bench`, "Flicker back and forth"): Alto 0,47% → 0,43%, 100% 0,29%.
- **Mip bias**: `log2(render / saída) - 1`, entre -3 e 0 (o -1 do FSR 2 e do DLSS: o acúmulo do jitter resolve o mip mais
  fino). Antes só o log2, entre -2 e 0.
- **Anisotrópico 16x** (o máximo da GPU quando menor) no sampler dos materiais, antes 8x.
- Custo: nenhum medível (casa assombrada, 1440p, GPU 4,19/4,21 ms antes e 3,79/4,65 ms depois, com outra sessão usando a
  GPU). No Alto (render a 67%) o padrão mais fino que o render alcança continua se perdendo mais cedo: é a resolução,
  não o filtro (Modo de Upscaling Nativo para ver tudo).

### Vista distante, segunda rodada (2026-11-23)
Medida nova: energia de detalhe (|laplaciano| da luminância) por faixa de distância do chão, nas capturas do mesmo
enquadramento (`--cam`; o `--cam` às vezes não pega num projeto novo: conferir a posição do eixo vermelho antes de
comparar). No Alto, faixa do meio: 3,3 (0.45) → 3,9 (primeira rodada) → **8,8** (esta); as faixas distantes 5,8/4,3 →
9,2/9,0, acima do que o render a 100% dava antes (7,6). Cintilação 0,44% (igual).
- **O M_Grid é procedural** (`MATF_WORLD_GRID`, `mesh.frag`), não textura: as linhas de 10 cm sumiam por `max(fw.x,
  fw.y)` (o eixo da profundidade do chão rasante apagava as duas direções), pela largura do pixel do RENDER (67%: 1,5x) e
  cedo (3,3 a 2,5 px). Agora a largura em pixel da SAÍDA (`tsr_size`), cada direção pelo seu eixo, sumindo de 4 a 2 px.
  Foi o ganho grande.
- **TSR**: a gaussiana em pixels da saída (em pixels do render, a 67% ela borrava 1,5x mais na tela), os pesos relativos
  à amostra mais perto; fases do jitter `8 × razão²` também no TSR (antes 8). Ganho ~10% no Alto. (Testado e
  descartado: misturar cada quadro pela confiança, `wnear / (frames + wnear)`: +10% de detalhe, mas o `ghost_medium` foi
  de 0,5% a 1,02%, acima do limite de 0,7%.)
- **Mips Kaiser** (`mip_kaiser.comp`, `tex_mips_hq`): sinc com janela de Kaiser (beta 4, 8 amostras por eixo), em linear
  para a cor base (o blit fazia a média em sRGB), wrap ou clamp (UDIM); buffer reservado antes de gravar
  (`mip_buf_reserve`). Só no bake de carregamento: no streaming (dentro do quadro) o `texture_streaming` caía de 200 para
  97 fps, lá ficam os blits. Neste chão quase nada (é procedural); vale para as texturas assadas de verdade.
- Testes: ssr, play_flicker, shadow_ghost, ghost_*, stable_pan*, upscaler_*, udim*, materials, material_lobes*,
  masked_lobes, texture_streaming, demo_*_flicker, assistant passando.

## FSR, DLSS e XeSS (`upscale_vendor.c`, 2026-11-09)
Os SDKs foram baixados com o ok do Nathan (licenças aceitas): `tools/fetch_upscalers.ps1` põe em `external/upscalers`
(fora do git; cada licença lá) o **DLSS 310.9.16** (headers, `nvsdk_ngx_d.lib`, `nvngx_dlss.dll` de release), o **FSR
3.1.4** (headers da FidelityFX API e a `amd_fidelityfx_vk.dll` assinada do SDK 1.1.4: o 2.x não tem DLL Vulkan pronta) e o
**XeSS 3.0.2** (`libxess.dll`, que se diz XeSS-SR 2.0.2). Sem eles a build tem só o TSR (`RE_HAVE_DLSS/FSR/XESS`). As DLLs e
as licenças (`*_LICENSE.txt`) vão para o lado do editor (POST_BUILD), do pacote (`make_dist.ps1`) e do jogo (Empacotar Jogo
leva a do upscaler do projeto). FSR e XeSS são carregados por `LoadLibrary` (só a DLL ao lado do executável); o DLSS pela
lib estática do NGX, que carrega a `nvngx_dlss.dll`.
- **Onde roda**: no lugar do `tsr.comp` (render.c "2c"). `ups_mv.comp` escreve antes os vetores de movimento completos
  (RG16F na resolução do render: a câmera pela profundidade + o passe de velocidade dos atores, como o TSR reprojeta), a
  profundidade vai para `SHADER_READ_ONLY_OPTIMAL` e o HDR de saída para `GENERAL`; depois o set e os push constants da
  engine voltam (o runtime deixa os dele). Falhou: TSR nesse quadro e nos seguintes, com o motivo no log.
- **Convenções medidas**: jitter com o sinal da engine (um ponto em q desenhado em q + jitter, em pixels do render; o sinal
  oposto: XeSS parado 0,69% → 1,44% de cintilação); movimento em pixels do render apontando para o quadro anterior
  (`-largura, -altura` sobre o uv atual − anterior; o sinal oposto dobrou as linhas da grade atrás da câmera andando nos
  três); profundidade invertida e infinita (FSR: `cameraNear = FLT_MAX`, `cameraFar = 2 cm`). Fases do jitter: 8 × (saída /
  render)², entre 8 e 64 (o TSR fica com 8).
- **Exposição**: a cor já sai exposta pela exposição automática da engine. A autoexposição de cada um brigava com ela (o DLSS
  ficava mais escuro e pulsava). Com um upscaler de fabricante, o quadro é iluminado com a exposição lida de volta (um ou
  dois quadros atrás: `FrameBuf.ups_exposure`, que `pre_exposure()` usa e o `exposure.comp` grava como a pré-exposição; o
  composite soma a diferença para a adaptada), e esse mesmo número vai como `InPreExposure` / `preExposure`: o histórico
  deles acompanha cada mudança.
- **Vulkan**: as extensões de instância e dispositivo que o NGX e o XeSS pedem (só as que a GPU tem; `VK_EXT_buffer_device_address`
  fica de fora, já é do 1.2), os recursos que o XeSS acrescenta na cadeia, e para o FSR `VK_KHR_get_memory_requirements2` /
  `VK_KHR_dedicated_allocation` (o backend dele chama o nome KHR sempre que a GPU lista a dedicated allocation: era um crash
  na criação) e `shaderFloat16` / `shaderInt16` (o caminho fp16 que ele escolhe). O HDR ganhou `TRANSFER_DST` (o DLSS limpa a
  saída no reset). Um aviso de validação interno do FSR (`rw_luma_history`: o shader dele RGBA8, a imagem dele RGBA16F) é
  filtrado no callback.
- **DLSS**: preset K (transformer) em todos os modos: na RTX 3060 custa 2,2 ms em qualquer modo, contra 4,1 ms do M
  (padrão do Desempenho) e 5,1 a 7,8 ms do L. Qualidade pela fração (≥ 0,95 DLAA, ≥ 0,62 Qualidade, ≥ 0,54 Equilibrado,
  ≥ 0,42 Desempenho, abaixo Ultra). Refeito (com a GPU parada) quando o render ou a saída mudam de tamanho.
- **FSR**: um contexto por tamanho de saída, com resolução dinâmica (qualquer render até a saída sem refazer).
- **XeSS**: a qualidade cuja faixa contém a fração (AA, Ultra Qualidade, Qualidade, Equilibrado, Desempenho, Ultra);
  refeito quando ela muda (o init dele é bloqueante: a GPU para um instante).
- `--upscaler tsr|fsr|dlss|xess` troca só na execução; `REALENGINE_NO_VENDOR_UPSCALERS=1` não liga as extensões deles;
  `REALENGINE_UPS_DEBUG=1` liga a checagem do FSR.

### Medidas (RTX 3060, Épico)
| Cena | TSR | FSR 3.1 | XeSS | DLSS (K) |
|---|---|---|---|---|
| house, 75% (1294×798 → 2156×1330): custo do upscale | 0,44 ms | 1,6 ms | 2,6 ms | 2,2 ms |
| house: quadro inteiro | 283 fps | 207 | 170 | 184 |
| heavy, Qualidade 67% | 69 fps | 64 | 63 | 62 |
| heavy, Desempenho 50% | 95 fps | 88 | 78 | 82 (o M padrão: 68) |
- O custo entra em **Reconstrução** no orçamento do quadro: o passo de upscale (TSR ou fabricante) ganhou os timestamps
  22-23 (`gpu_upscale_ms`; antes ele caía em "Volumes, água e céu", junto com o céu).
- Cintilação parada (house, 3 s, a exposição ainda subindo desde o início): TSR 0,07%, FSR 0,45-0,57%, XeSS 0,6-0,7%, DLSS
  4-5% (em 20 s, com a exposição quase parada: 0,9%). Já estável, a mudança média por quadro (0-255): FSR 0,017, TSR 0,029,
  DLSS 0,104, XeSS 0,112. O DLSS acompanha pior a subida da exposição mesmo com a pré-exposição: fica anotado.
- Testes: `upscaler_dlss`, `upscaler_fsr`, `upscaler_xess` (house com validação: ativo, sem VUID, cintilação abaixo de 8 /
  1,2 / 1,5%); só existem quando a build tem o SDK.
- Falta: a máscara de reativos (translúcidos e partículas antes do upscale: hoje eles são desenhados depois, na resolução
  da saída), escolher o upscaler no menu de opções do jogo, o FSR 4 (só DX12 por enquanto) e o XeSS nativo nas Arc (XMX:
  medir numa placa Intel).

## Medidas (RTX 3060, 2560×1417, Épico, cena heavy, 5 s)
| Modo | Render | fps | GPU ms | Geometria | GI |
|---|---|---|---|---|---|
| Nativo | 100% | 47 | 21,12 | 17,03 | 2,72 |
| Predefinição (Épico) | 75% | 64 | 15,38 | 11,63 | 2,60 |
| Qualidade | 67% | 71 | 13,92 | 10,32 | 2,51 |
| Equilibrado | 58% | 83 | 12,00 | 8,67 | 2,33 |
| Desempenho | 50% | 97 | 10,26 | 7,08 | 2,25 |
| Ultra Desempenho | 33% | 131 | 7,53 | 4,42 | 2,23 |
| Qualidade + dinâmica 100 fps | 67→35% | 114 | 8,66 | | |
- A geometria da heavy (2,1 M triângulos, 600 k cartões Masked) cai com os pixels: é sombreamento, não vértices.
- Testes: `upscale` (modos, retorno ao TSR, a dinâmica convergindo num GPU simulado: 70% com 8,35 de 10 ms, sem oscilar),
  `budget_heavy_dynres` (heavy no Épico, Qualidade + dinâmica 100 fps, cabe nos 10 ms).

## Upscaler no menu de opções do jogo (2026-11-09, sobras da Fase 2, 4.4a)
- Game User Settings `Upscaler` (GameUserSettings.ini): -1 o do projeto, ou um `UPS_*`; as escolhas no combo são
  "Padrão do Projeto" e cada upscaler que roda ali (`upscale_available`: o runtime instalado, a GPU). O jogo aplica por
  `render_set_upscaler_override` só quando muda (um `--upscaler` da linha de comando fica).
- WBP_Options: linha "Upscaler" na aba Gráficos; um WBP_Options feito antes ganha a linha no fim da página ao abrir o
  projeto (como os botões de salvar no WBP_PauseMenu).
- Teste `pause_options`/`pause_options_reload`: o combo escolhido (TSR) volta no reload.
- Falta: máscara de reativos.

## No viewport do editor (2026-11-10, "os upscalings devem funcionar dentro da viewport do editor")
O upscaler escolhido (TSR, FSR, DLSS, XeSS) já valia no editor, mas o **Modo de Upscaling** e a **Resolução Dinâmica**
das Configurações do Projeto só no jogo. Agora o laço do editor (`main.c`) faz o que o `game_main.c` faz: guarda a parte
que a escalabilidade pôs (percebe quando ela muda), aplica a do modo por cima e a resolução dinâmica por baixo. Sem
`--upscale-mode`/`--dyn-res` na linha de comando (essas continuam valendo para medidas). Os testes não mudam (projeto
padrão: Predefinição, sem resolução dinâmica).

## Resolução dinâmica sem defeitos na troca (2026-11-14, "Dynamic Resolution cria bugs na tela")
- Cada passo da resolução dinâmica (5%, até 2 por segundo) chamava o `ensure_targets` como um redimensionamento
  completo: espera da GPU, todos os alvos refeitos (o denoiser realocava 361 → 152 MB no meio do jogo) e **todos os
  históricos zerados**: um quadro serrilhado e com ruído (TSR sem histórico), a GI do denoiser com ruído de novo, as
  nuvens borradas por um segundo, uma travada.
- Agora, quando só a fatia do render muda (a saída, o TSR e as amostras iguais): o que é do tamanho da saída fica (a
  imagem, a máscara do contorno, o histórico do TSR, o bloom) e o TSR **não** recomeça; o denoiser e a subsuperfície
  da pele reaproveitam as imagens enquanto o render cabe nelas (os passes usam o tamanho do render, não o da imagem);
  o denoiser reprojeta o histórico com o tamanho do quadro anterior (`push.instances` no `denoise.comp`); as nuvens de
  tela guardam o tamanho enquanto o render fica a menos de 30% dele (os passos de 5% não as reiniciam; pelo tamanho da
  saída custavam +0,1 ms no Épico). Aviso no log: "Resolucao dinamica: render A -> B".
- Medida (MyProject, Vídeo-realista, `--upscale-mode 1 --dyn-res 80`, 10 s): 4 trocas sem realocar o denoiser (a
  0.31: 2 realocações); cintilação 5,06 → 5,02%; testes upscale, budget_heavy_dynres, play_flicker, house_walk,
  denoiser_*, render_style_video, game_fps ok.
- Falta (o caminho da Unreal): alvos fixos no tamanho máximo e o render num sub-retângulo, sem nenhuma espera na troca.

## DLSS e resolução dinâmica (2026-11-14, "o DLSS tá bem bugado: a imagem fica diferente, flickando, muito ghosting"; "a resolução dinâmica fica com um blur bem feio")
- **Exposição do DLSS**: ele divide a cor pela `InPreExposure` e tonemapeia por dentro pela exposição dele (1 sem
  textura): as unidades físicas da engine (sol em lux) ficavam dezenas de milhares de vezes acima da faixa, o histórico e
  a rejeição dele trabalhavam numa imagem estourada. Agora `InExposureScale` = a mesma pré-exposição.
- **DLSS com resolução dinâmica**: era refeito (GPU parada, histórico perdido) a cada passo; agora é criado para o maior
  render visto e a saída, e um render menor vai como subretângulo (`InRenderSubrectDimensions`).
- **Piso da resolução dinâmica**: `upscale_dynamic_floor` = 70% da parte do modo, nunca abaixo de 50% da saída (antes
  metade da parte: Qualidade 67% descia a 33%). **Nitidez** do composite mais forte quanto menor o render: 0,2 nativo,
  ~0,5 a 75%, 0,9 a 50% ou menos (presa à faixa da cruz: sem halos).

## Mínimo da Resolução Dinâmica (2026-11-16)
- Project Settings > **Dynamic Resolution Min** (`dynamic_resolution_min`, % da saída; 0 = Automático) e
  `--dyn-res-min`. É o MinScreenPercentage da Unreal. No Automático vale o piso de antes: 70% da parte do modo, nunca
  abaixo de 50%.
- O `budget_heavy_dynres` falhava desde o piso de 50% (a heavy no Épico a 50% dá 10,5–10,8 ms). Agora usa 35%:
  67 → 35%, GPU 9,08 ms de 10.

## As escolhas do Upscaler num thread (2026-11-25, bug-fixer)
- A 1ª pergunta a cada runtime (DLSS: o NGX, ~1,2 s) vinha no começo do Play (`gus_warm`, Q22) e travava o quadro do
  Play (1224 ms no escopo, 1284 ms na varredura). Agora `ups_vendor_warm_async` pergunta num thread próprio (prioridade
  baixa); `ups_vendor_usable` tem uma trava: quem perguntar no thread principal antes da resposta espera, como antes.
  O `ups_vendor_shutdown` espera o thread. Teste `play_hitch` (pior quadro do Play 1266 -> 127 ms num Terceira Pessoa
  novo; o que sobra é o 1º quadro de jogo).
