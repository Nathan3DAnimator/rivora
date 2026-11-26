# Água: chuva, água rasa e oceano (2026-11-08, pedido do usuário)

Pedido: "chuva igual a do Tlender (pinga, respinga, escorre, cai, cria poças e molha os modelos), realista e otimizada
para tempo real; chuvas pesadas que criem quase rios; e um sistema de líquidos opcional, o mais avançado, realista e
otimizado que existe, possibilitando oceanos que reagem em tempo real sem quedas de fps". O usuário com controle total.

## Arquivos
- `src/water.h/.c`: o módulo (buffers, o espectro na CPU, a janela que segue a câmera, passadas, desenhos, a altura do
  oceano para os barcos, autoteste). `CRain` (componente novo, `COMP_RAIN`) e `CWater` crescido (Ocean, vento, ondulação,
  agitação, espuma, cores, transparência, interação; níveis antigos abrem com os padrões).
- `shaders/water.glsl` (dados, amostragem, o molhado das superfícies), `water_sim.comp` (passadas, versão com e sem ray
  query), `water_surface.vert/.frag` (a superfície), `rain.vert/.frag` (gotas e respingos). Tudo em buffers por endereço
  (`GpuFrame.water_addr[8]`, `water[40][4]`): nenhum descritor novo.

## Como funciona
- **A superfície da chuva**: um mapa em volta da câmera (`Area` m, `Resolution` células; padrão 96 m / 512 = 19 cm),
  janela toroidal (a célula do mundo (x, y) vive em (x mod N, y mod N): andar não copia nada; uma célula nova na janela
  zera a água dela). 1/8 das linhas por quadro: um raio de cima até a primeira superfície (TLAS estático + dinâmico).
  Sem ray query: chão aberto na altura da câmera (molha, sem poças).
- **Água rasa** (tubos virtuais, Mei, Decaudin e Hu 2007), 2 meio-passos por quadro: fluxo pelas 4 faces pela diferença
  de nível (chão + micro relevo + água), limitado ao que a célula tem; chuva (mm/h × ganho), infiltração e evaporação;
  a velocidade do fluxo (aparência dos córregos); o molhado sobe enquanto chove e cai depois (água parada mantém).
- **Superfícies** (`rain_shade` no `mesh.frag`): exposta se acima da superfície da chuva (parede: a célula do lado de
  fora); molhado à la Lagarde (difuso mais escuro, mais liso); escorridos nas paredes; poças e córregos com normal das
  gotas e o fluxo carregando os anéis. **Os anéis são assados** num mapa 128² que repete a cada 128 cm (passada 4).
- **Gotas e respingos**: só hash, sem estado; caixa de 50 × 20 m presa ao mundo; escondidas abaixo da superfície da chuva.
- **Oceano** (Tessendorf): espectro JONSWAP (pico limitado ao de Pierson-Moskowitz) com espalhamento cos^2s, 3 cascatas de
  256² (400 m, 60 m, 8 m), cada uma com sua faixa de comprimentos; IFFT em memória compartilhada (uma linha por grupo, 8
  estágios radix-2), linhas e colunas; deslocamento com agitação, inclinações, Jacobiano → espuma que decai.
  Superfície: anéis polares em volta da câmera (200 × 256, de 30 cm a ~20 km), cada cascata some quando suas ondas ficam
  menores que ~4 vértices; Fresnel, céu refletido, GGX mais áspero longe, cor funda, luz pelas cristas, espuma.
- **Lagos** (Water Body sem Ocean): grade 128² no retângulo, as mesmas duas ondas que os barcos sentem.
- **Interação**: equação de onda 256² (25 cm) em volta da câmera, empurrada por veículos, personagens e corpos com física
  na água (até 16).
- **Barcos**: `water_ocean_height` soma as 64 maiores ondas das cascatas 0 e 1 (as que importam para um casco).

## Neve (2026-11-08, "ajuste o que falta")
- O mesmo ator Rain com **Snow** ligado (`CRain.snow`, `flake_size`, `cover_time`, `cover_angle`, `melt_time`,
  `start_cover`; o componente cresceu no fim, níveis antigos abrem sem neve).
- **Cobertura** no `w` da superfície da chuva (0..1, por célula): cresce no `pass_water` enquanto neva (mais rápido com
  mais intensidade), derrete depois (a água entra na água rasa: ~0,5 cm por cobertura cheia). Fica enquanto a célula é a
  mesma; uma célula nova na janela e tudo fora da janela usam a **cobertura do aberto** (`water[38].z`, a mesma conta na
  CPU). Sem molhado nem poças enquanto neva.
- **Aparência** (`snow_shade`): branco fosco onde a normal geométrica sobe o bastante (`Cover Angle`), deriva suave por
  duas oitavas de ruído de valor (manchas mais finas enquanto junta); a normal vira para cima (relevo na normal fazia
  riscos escuros). Embaixo de telhados: a mesma exposição da chuva.
- **Flocos** (`rain.vert`, tipo 2): caixa menor (26 × 14 m) e 1,5× mais que gotas, 60-100 cm/s, balançando, discos
  virados para a câmera de ≥ 2 px, luz do céu e do sol espalhada (o mesmo brilho da neve no chão).
- Cena `--scene snow` (o quintal da chuva sob 30 mm/h de neve, cobertura em 4 s), teste `snow_scene` (≥ 200 fps).
- Pegadinha: a 1ª execução depois de mudar shaders refaz o cache de pipelines (um quadro de ~30 s); o relógio da água
  limita o passo a 0,1 s, então a captura saía sem neve. Medir sempre numa 2ª execução.

## Pegadas na neve (2026-11-08, "vamos fazer o que falta")
- **Rastro**: um buffer de 512 × 512 células de 4 cm (~20 m) em volta da câmera, como a janela da chuva (célula em
  `(x mod N, y mod N)` com a chave do lugar: anda sem copiar), `vec2` (fundura, chave). `GpuFrame.water` cresceu para 56:
  48 o endereço (bits) e a 1ª célula, 49 ligado / carimbos / dt / reenchimento, 50..55 os carimbos.
- **Passadas** (CPU, `water.c`): até 16 Characters acompanhados pelo handle; a cada 38 cm andados no chão um carimbo,
  alternando os pés 11 cm de cada lado do caminho, virado para onde anda (pulo e teleporte não marcam).
- **`pass_trail`** (passe 14): a bota (sola na frente, calcanhar atrás, elipses com borda suave) afunda; fora dela, uma
  borda de neve empurrada (fundura negativa) só onde não há pegada. A neve que cai enche de volta no ritmo da cobertura.
- **Aparência** (`snow_shade`): bilinear das 4 células, o gradiente vira relevo na normal (fundura ~7 cm × cobertura),
  a neve pisada mais cinza (compactada, e as paredes da pegada sombreiam o fundo). Só lido em chão (face > 0,5).
- Cena `snow`: um Walker dá voltas (`editor_snow_tick`); o teste `snow_scene` exige pegadas
  (`Water: ... N footprints`).
- Medida (RTX 3060, `--scene snow`, 1440p): GPU 2,21 → 2,23 ms (+0,025 ms: o passe e as 4 leituras no chão nevado).
- Falta: rastro de rodas de veículos, neve acumulando em altura de verdade (malha deslocada).

## Lagos transparentes e espuma na margem (2026-11-08)
- Cada lago agora tem **cor, transparência, cor do espalhamento e Espuma na Margem** próprias (`water[40..47]`, o
  `GpuFrame.water` cresceu para 48); antes os lagos usavam os números do oceano (zero sem oceano: preto).
- `water_surface.frag` com `RE_RT` (variante escolhida com ray query; `REALENGINE_NO_WATER_RT=1` para medir sem):
  em Alto/Épico (`GI_RT | GI_REFLECTIONS`, a mesma classe dos reflexos traçados) e até 150 m, um raio na direção
  refratada (n = 1,33) acha o fundo (até 3 × a transparência). A luz do fundo: o cache de superfície (como os reflexos),
  senão céu + sol com a sombra virtual; a cor: a média do material (`GpuMaterial.avg`) ou a do ator. Beer-Lambert por
  canal (`exp(-t / clareza × (2,2; 1; 0,75))`: o vermelho some primeiro) na descida da luz e na subida até o olho; o
  resto é a cor funda. Espuma onde o fundo fica a menos de ~35 cm (margens, pedras, postes), quebrada por dois ruídos
  que andam. O oceano faz o mesmo (praias), com a espuma em `water[7].w` (1 + Espuma na Margem).
- Cena `--scene lake` (lago raso, praia, pedras, píer, um piso no fundo), teste `lake_scene`.

## Medidas (RTX 3060, 1440p, GPU mínima)
| Cena | Com | Sem | Custo |
|---|---|---|---|
| `--scene rain` (60 mm/h, 36 mil gotas, 3 mil respingos, 512²) | 1,80 ms | 1,44 ms | 0,37 ms |
| `--scene ocean` (vento 11 m/s, 3 cascatas, 300 mil vértices) | 1,86 ms | 1,10 ms | 0,76 ms |
| `--scene snow` (30 mm/h, 27 mil flocos, cobertura, 512²) | 1,76 ms | 1,45 ms | 0,31 ms |
| `--scene lake`, água na tela toda: transparência + espuma (raio) | 1,95 ms | 1,67 ms (sem raio) | 0,23 ms (+0,05 do shader maior) |
| `--scene ocean`: o mesmo no oceano (os raios não acham fundo) | 2,03 ms | 1,89 ms | 0,15 ms |
| `many_lights` sem chuva (o custo da chuva no `mesh.frag`) | 10,8-11,0 ms | 10,98 ms (0.23) | ~0 |
- A primeira versão do `rain_shade` (anéis calculados no pixel) custava +0,9 ms em `many_lights` mesmo sem chuva
  (registradores); com `[[dont_flatten]]` +0,3; com os anéis assados no compute, ~0.
- Autoteste `water`: Hs 0,58 m a 5 m/s e 4,2 m a 12 m/s (mar desenvolvido: 0,53 e 3,1 m; o JONSWAP dá mais no pico).
  Testes `rain_scene` e `ocean_scene` (validação, ≥ 200 fps).

## Bugs achados
- `%` com negativos é indefinido no GLSL: só o lado positivo do mapa tinha água. Agora `floor` ou `&` (potência de 2).
- O relógio da água andava só com Post Process Volume na cena (campo errado): agora uma vez por quadro.

## Fundo com textura e cáusticas (2026-11-08, "vamos fazer o que falta")
- O raio refratado (`water_surface.frag`, variante RE_RT) agora lê o **material do fundo**: a cor base assada no UV do
  ponto (`Hit.uv`, novo em `rt.glsl`), World Aligned pelo eixo dominante, as linhas do M_Grid (`MATF_WORLD_GRID`, como
  as do `mesh.frag`, com o cone no lugar do `fwidth`); animados ficam na cor média. O mip vem de um **cone de raio**: o
  ângulo do pixel × (distância até a água + caminho dentro dela) / cos no fundo, vezes a densidade de UV do triângulo
  atingido (`Hit.uv_density`: √(área UV / área no mundo); o compilador descarta onde não é lido). Antes, um mip fixo
  apagava as linhas finas.
- **Cáusticas**: duas camadas de células (F2 − F1 de Worley 3 × 3, o ponto de cada célula na metade do meio, senão
  3 × 3 nem sempre acha o 2º mais perto e aparecem cortes) com dois oitavos de distorção que derivam (bordas curvas;
  retas pareciam ladrilho), células de ~90 cm. Média ~1 (a luz só muda de lugar), nítidas a poucos cm e espalhadas no
  fundo (`exp(-d/600)`), nada sem sol. Multiplicam só a luz **direta** do cache de superfície (o sol): uma leitura de
  sombra a mais por pixel custava 0,25 ms e foi tirada.
- `lake_scene`: o piso de ladrilhos do fundo com M_Grid.
- Medidas (RTX 3060, `--scene lake`, Épico 1440p, água rasa na tela toda): 3,15 → 3,50 ms (+0,35: textura e grade
  +0,06, cáusticas +0,08, o resto o shader maior); Alto e abaixo sem mudança (2,04 ms, sem o raio). Com a sombra extra
  era 3,75; com a 2ª camada de 4 células 3,36, mas com cortes visíveis (descartada).

## Falta / próximos passos
- Fluido 3D (SPH/PBF/MLS-MPM) localizado: não feito; o oceano e a água rasa cobrem rios, enchentes e mar.
- Refração do que não está no TLAS (partículas, translúcidos) e cáusticas de luzes locais (só o sol).
- Neve acumulando em altura (malha deslocada; as pegadas já existem) e molhado próprio de atores que se movem.

## Memória mapeada (2026-11-08, plano de melhorias A0d)
Revisão do mesmo erro que derrubava a GPU no tecido (CPU reescrevendo memória host-visible que o quadro em voo lê):
- **Espectro do oceano (`h0`)**: era uma cópia só, reescrita por `make_spectrum` quando o vento muda, enquanto o quadro
  anterior ainda podia lê-la (ondas rasgadas por um quadro; não derruba, são floats). Agora o espectro é montado na CPU
  (`h0_cpu`) e copiado para o buffer do slot do quadro (`h0[RE_FRAMES_IN_FLIGHT]`, 3 MB cada) só quando muda.
- `water_shutdown` destruía 8 dos 9 buffers: o das pegadas (`trail`) vazava. Corrigido.
- O resto conferido: chuva, superfície, fluxo, ondulações e rastros são device-local; o `GpuFrame` já é por slot.
- Fora da água: partículas (blocos e leituras por slot, forma por staging com espera), skin (paleta por slot), cache de
  superfície (realoca no máximo a cada 8 quadros, depois dos 2 em voo), bake (espera a GPU), malha de tecido (espera a
  GPU) e tabelas das nuvens (escritas uma vez) estão certos. O volume de GI carregado (`gi_volume_load`) agora espera a
  GPU antes de reescrever o buffer (carregar nível: raro).

## Rios, flutuação, nado e debaixo d'água (2026-11-09, plano de melhorias B12c, B12f, B12h)
- **Rios (B12c, `src/river.c`, `COMP_RIVER`):** o Water Body River da Unreal. Um ator com Water Body (cor, clareza, espuma)
  e River (até 32 pontos no espaço do ator, cada um com largura, profundidade e velocidade). Amostras Catmull-Rom (8 por
  segmento, até 256) com a direção do fluxo; `river_at` dá a altura, a correnteza (mais lenta nas margens) e a
  profundidade num ponto. Desenho: `water_surface.vert` passe 200+ (uma fita de 16 quads de largura pelas amostras, no
  buffer do quadro em `pc.instances`: o `pc.vertices` é o das malhas que o `rt.glsl` lê nos acertos dos raios; com ele
  no rio a GPU caía) e no `.frag` um flow map (ruído de valor em duas escalas arrastado pela correnteza em duas fases).
  Achado: o ramo do oceano (`pass >= 100`) vinha antes e pegava os rios; e um `lake ? water[40 + pass] : ...` lia
  fora do buffer do quadro com o passe 200 (os dois lados da escolha são lidos).
- **Consulta de água (`water_query`, vehicle_sim.inc):** lagos, oceano e rios; altura, correnteza, densidade e
  profundidade num ponto. `water_gather` uma vez por quadro (física e personagens).
- **Buoyancy (B12f, `COMP_BUOYANCY`, physics.c):** num corpo com física, 5 pontões (cantos de baixo e o meio da caixa)
  erguidos pela água sobre cada um (fração submersa × volume × densidade × g × Buoyancy), no seu lugar (o corpo endireita),
  velocidade puxada para a correnteza e giro freado; um corpo em água corrente fica acordado.
- **Nado (B12f, `CCharacter.swimming`, bp_vm.c):** água acima de 1,3 × a meia altura sobre os pés: sem gravidade, o
  centro puxado para o nível − 0,45 × a meia altura (a cabeça fora), 55% da velocidade (ou Swim Speed), a correnteza
  leva, Jump sobe. Água acima dos joelhos: anda mais devagar.
- **Debaixo d'água (B12h, composite.frag `RE_POST`):** `water.c` acha a água em que a câmera está (oceano, lago, rio) e
  põe o nível, a cor e a clareza em `water[50..51]`; o render usa a variante do composite com profundidade. Por pixel: o
  ponto no plano próximo abaixo do nível (a linha d'água corta a imagem com a lente na superfície), uma leve
  ondulação, absorção pela distância (vermelho 0,18, verde 0,6, azul 1 × a clareza) para a cor da água iluminada pelo céu
  e pelo sol, mais escura com a profundidade. `REALENGINE_NO_UNDERWATER=1` desliga.
- **Medidas** (3060, Épico): cena `river` em Play 307-311 fps (o rio, as 3 caixas e o nadador), `underwater` 291 fps.
  Testes `river` (as 3 caixas flutuam e descem ≥ 1,5 m, o personagem nada) e `underwater`.
- **Falta:** margens/leito do rio esculpidos no terreno, cachoeiras, os barcos (veículos) sentirem a correnteza dos rios,
  animação de nado, a superfície vista de baixo com a janela de Snell, cáusticas no fundo do rio (A9.5c).

## Chuva inclinada, borda suave, goteiras e escoamento (2026-11-10)
- **Grade inclinada pela chuva:** o mapa da primeira superfície é traçado ao longo da direção da chuva (vento / velocidade
  de queda: 6 m/s a gota, 0,8 m/s o floco, no máximo 1,5 de inclinação); um ponto fica na célula `xy + inclinação * z`
  (`water.glsl rain_slant / rain_u`, `water.c` a mesma conta para a janela). Com vento a chuva entra sob o beiral do lado
  do vento; o lado de trás da parede fica seco.
- **Sem quadrados, borda suave:** a exposição é a média das 4 x 4 células por um B-spline cúbico (`rain_exposure`): a
  borda molhado/seco tem ~2 células, sem degrau nem faixa (antes: a célula mais próxima, quadrados de ~12 cm).
- **Goteiras:** 1 em 6 gotas perto da câmera sorteia uma célula e um lado; se é uma borda com água (40 cm ou mais acima
  da vizinha), cai uma gota da borda até a superfície de baixo, em ciclo (`rain.vert`).
- **Escoamento:** a água que entra numa célula pelos canos virtuais molha a célula pela vazão (`in_ / area`: ~2 mm/h já
  escurece, 20 mm/h encharca) e um filme de 0,3 mm conta como molhado (antes 0,8 mm): o caminho da água e o plano onde ela
  se espalha ficam molhados enquanto ela corre.
- Medida (`--scene rain`, 1440p, RTX 3060, com validação): 338 fps, GPU 2,92 ms.

## O que se move na chuva, goteiras visíveis, quantidade e velocidade (2026-11-10, pedido do usuário)
- **Rastro seco (bug):** a superfície da chuva era traçada contra o TLAS estático e o dinâmico; debaixo de um personagem
  o chão virava "coberto" na hora (o quadrado seco sob a Delight) e o rastro durava até a linha ser traçada de novo.
  Agora a superfície (`x`) é só a do cenário estático (molhado, água rasa, exposição); o que se move fica na **segunda
  metade do buffer** (`rain_mover`: velocidade xyz e topo w), onde as gotas param (`rain_top`) e os respingos saem.
- **Velocidade no ponto atingido** (`water_sim.comp first_surface`): a da animação (os vértices do skin do quadro
  anterior, os mesmos do velocity pass: `lightmap_st.x`) + a do ator (`water[57..62]`: os 3 que se movem mais perto da
  câmera, posição/raio e velocidade pela diferença de posição; `water[56]` = quantos, dt, Fall Speed; `GpuFrame.water`
  cresceu de 56 para 64). Um respingo num ator com mais de 30 cm/s vira um **pingo lançado** (velocidade × 0,75..1,25
  + um pouco para cima, cai em arco, desenhado como risco na direção do movimento).
- **Goteiras visíveis:** 1 em 4 gotas perto da câmera procura até 3 células × 4 lados por uma borda com água (≥ 30 cm
  acima, chão plano depois: com vento a chuva inclinada via a face de uma parede como rampa e punha fileiras de gotas no
  meio dela). Ciclo: a gota cresce pendurada, cai esticando, coroa onde bate. Desenho `kind 3` no `rain.frag` (conta
  clara, borda mais escura, brilho), pelo menos ~3 px (antes ~1,6 px: ninguém via).
- **Parede espelho (bug, "o chão aparenta estar à frente da casa"):** os escorridos levavam a rugosidade da parede a
  0,03 e ela refletia o chão molhado como se ele atravessasse a base. Parede molhada agora tem brilho (≥ 0,14), não espelho.
- **Drop Amount** (0..4×, quantas gotas) e **Fall Speed** (0,25..3×, a velocidade e a inclinação no vento) no `CRain`
  (cresceu no fim: níveis antigos abrem com 1×). `MAX_DROPS` 60 mil → 160 mil.
- Cena `rain` ganhou a escada e a sala de prototipagem e um Walker andando em círculo (o `editor_snow_tick`).
- Não reproduzido: as manchas pretas na escada do usuário (blocos do tamanho das células); a hipótese é o reflexo de
  poça/parede quase espelho num lugar sem reflexo bom (a mudança das paredes deve reduzir). Falta o nível dele para fechar.
- Medida (`--scene rain`, 1440p, RTX 3060, com a escada, a sala e o Walker): 322-335 fps, GPU 2,96-3,06 ms.

## Water Body Exclusion: áreas sem oceano (2026-11-10, "colocar áreas que a água não pode sobrepor, tipo áreas de terra")
- Como o Water Body Exclusion Volume da Unreal, mas sem componente novo (o ECS está nos 64): um **Water Body** com
  **Exclusion** ligado (`CWater.exclusion`, cresceu no fim) é um retângulo (escala × 100 cm, girado pelo yaw) onde o
  oceano não existe. Adicionar Rápido > Mundo > **Water Body Exclusion** (20 × 20 m onde a câmera olha).
- GPU: até 8 por quadro em `water[64..79]` (centro, meias medidas; cos e sin do yaw), quantos em `water[63].x`
  (`GpuFrame.water` 64 → 80); o `water_surface.frag` descarta o oceano dentro. CPU: `water_excluded`, e
  `water_ocean_height` responde "sem água" ali: barcos, flutuação, nado e a câmera debaixo d'água seguem; o
  `gather_water` dos veículos ignora a exclusão (não é água).
- Cena `ocean`: um poço seco de 20 × 20 m com piso 2,5 m abaixo do mar. Autoteste `water`: dentro sem oceano, fora com.
- Falta: espuma na borda da exclusão e o formato livre (polígono / spline, a ilha desenhada).

## A chuva no chão sumia com a câmera alta (2026-11-14)
- Pedido: "se tiver chovendo, a chuva no chão some quando a câmera sobe". Duas causas:
  1. A janela de 96 m da chuva era centrada na grade inclinada pelo vento na **altura da câmera** (`u = xy + inclinação ×
     z`): com o vento padrão (3 m/s, inclinação 0,5) a câmera a 50 m deslocava a janela 25 m, a 100 m tirava o chão
     visto dela. Agora o centro é o **chão sob a câmera**: a altura da célula do meio do próprio mapa da chuva, lida de
     volta da GPU (16 bytes por quadro em voo, `ground_rb`, lida quando o slot volta), amortecida; até lá, os pés da
     câmera. O raio de cima alcança do topo até 100 m abaixo desse chão (câmera a 300 m de altura ainda o acha).
  2. Fora da janela o chão ficava **seco** (só a neve tinha o valor de "campo aberto"): de cima, um quadrado de chuva em
     volta da câmera. Agora o chão aberto lá fora molha como uma célula plana (`open_wet`/`open_water` na CPU: a água
     parada pela chuva × ganho menos a infiltração; uma película de 0,3 mm já molhada; seca depois), `water[56].w`;
     as poças somem nas últimas 8 células da janela (lá fora não há poças: sem borda).
- Testes da área `agua` 7/7. Custo: uma cópia de 16 bytes por quadro.

## Chuva até o chão do alto, atores molhando devagar, design, som e Blueprints (2026-11-14, pedido do usuário)
Pedido: "a chuva ainda some se a câmera subir muito"; "objetos molhados secam muito rápido ao sair da chuva, e molham
também muito rápido"; "mais escolhas de design"; "o som da chuva também ter opções"; "toda a chuva modificável por
blueprints".
- **Do alto** (`Far Rain`, 0..1): as gotas vivem numa caixa de 50 × 20 m em volta da câmera e os respingos a 18 m dela;
  a 30 m de altura nada chegava ao chão. Acima de 8 m do chão sob a câmera, metade das gotas cai numa segunda caixa em pé
  no chão onde a câmera olha (lado 1,5 × a altura, 50 a 300 m; `water[81]` centro, chão, lado; `water[83].z` o quanto)
  e metade dos respingos vai para lá (fora do mapa: chão plano na altura do chão), com tamanho mínimo de ~2 px e riscos
  de ~12 px.
- **Molhado por ator** (`wet_update`): Characters, Skeletal Meshes, veículos e corpos com física (até 64, os mais perto
  da câmera dentro da Área) têm o próprio molhado. A célula do mapa da chuva sobre a cabeça de cada um (e sobre a câmera)
  é lida de volta como o chão (16 B cada, um buffer por quadro em voo): aberto → encharca em `Actor Wet Time` (60 s, ×
  intensidade/20), coberto ou sem chuva → seca em `Actor Dry Time` (480 s). Para a GPU: um byte por índice de entidade
  (tabela host de 1 MB por quadro em voo, só os índices escritos são limpos), o ator e tudo o que ele possui (partes,
  peles, componentes, até 3 donos acima); `mesh.frag` lê o byte e usa o molhado próprio (sem poças) no lugar do da
  célula. Antes o personagem pegava o molhado da célula onde estava: secava e molhava na hora ao passar sob um telhado.
- **A chuva parando não seca tudo na hora**: o mapa continua enquanto o chão aberto ou um ator ainda está molhado
  (secam pelos tempos deles); antes `Rain Intensity = 0` desligava tudo no mesmo quadro.
- **Design**: `Preset` (Garoa, Fraca, Moderada, Forte, Tempestade, Monção, Estilizada: visual e som juntos; mexer depois
  vira Custom), `Drop Color`, `Streak Length`, `Drop Width`, `Splash Size`, `Gusts` (o vento das gotas oscila; a posição
  delas usa o caminho integrado `water[84].zw`, sem saltos; a água no chão segue o vento médio).
- **Som**: `MS_RainStyled` (novo: o `MS_Rain` salvo num projeto mantém as entradas dele) com Tone, Drop Pitch, Drop Rate,
  Drop Level, Muffle e Volume; `Sound Style` (Chão Aberto, Telhado, Zinco, Folhas, Lona, Distante), `Sound Volume`,
  `Muffle Under Roof` (a câmera sob telhado pela mesma leitura), `Custom Sound` (um som do projeto no lugar). Trovões
  (`Thunder`, `Thunder Volume`, `MS_Thunder`: estrondo de ruído marrom + estalos, envelope pela CPU, atraso pela
  distância) e `Lightning` (clarão no composite, `water[85].x`), só no Play.
- **Blueprint**: Set/Get Rain Property (qualquer campo pelo nome dos Detalhes, sem espaços nem caixa: tabela
  `g_rain_props` com faixas), Set Rain Preset, Set Rain Drop Color, Get/Set Actor Wetness.
- `GpuFrame.water` 80 → 88. Autoteste `water`: campos por nome e predefinição.

## Shading realista da chuva, estilos e água na lente (2026-11-15, pedido do usuário)
Pedido: "o shading não tá realista, quero algo realmente realista, tanto em tempestades quanto nas gotas, poças, água
escorrendo, splashes e goteiras; no Tlender tá bem realista, pode pegar de referência; permitir o shading da chuva ser
alterado, para o dev escolher o estilo e poder modificar como quiser"; depois "águas na tela (opcional ao dev), a tela
reagir igual uma câmera real gravando na chuva com água escorrendo pelo visor". Referência: `kernel_rain.h` do Tlender.
- **Duas camadas** (como o "molhar todos os materiais" do Tlender): o material molhado pela porosidade (`rain_porosity`:
  rugoso e não metal = poroso) fica mais escuro e saturado (`pow(base, 1 + 1,5k) × (1 − 0,35k)`) e o lóbulo próprio
  afia (rugosidade até 0,12-0,3); por cima uma **película d'água** com normal e rugosidade próprias (`g_rc`, `g_rcn`,
  `g_rca` no `water.glsl`): no `mesh.frag` um lóbulo dielétrico (F0 0,02) no `brdf` e no reflexo indireto (o raio
  refletido, o SSR e o céu vão pela normal da película; o denoiser não leva o reflexo de quem tem película > 0,25).
  Antes o chão inteiro virava espelho com a normal das ondas e o relevo sumia; agora a textura e o relevo aparecem sob
  um espelho nítido.
- **Poças**: a película grossa; borda pela profundidade + ruído (`Puddle Edge`, sem a forma das células), o chão sob a
  água mais escuro e tingido (`Puddle Color`, `Puddle Clarity`) e alisado pela refração. **Respingos nas superfícies**
  (`rain_impacts_on` do Tlender: coroas esticadas e jogadas para a frente pelo ângulo da chuva + pontos molhados, até 15
  m). **Filetes** nas paredes (o ruído do Tlender no lugar das listras retas). **Gotas nas paredes e janelas**
  (`rain_drops` do Tlender: paradas e escorrendo com rastro, só do lado que a chuva alcança, até 25 m); nos materiais
  Translucent (`rain_glass`) as gotas e a opacidade delas.
- **Gotas caindo** (`rain.vert drop_light`): o mundo refratado (média do céu e do chão) + o sol e as luzes da grade
  (até 12, a 35 m) espalhados para a frente (Henyey-Greenstein 0,82 + −0,35: Garg e Nayar 2006), nuvens tapam o sol,
  o relâmpago acende todas. Contra o céu claro a gota some (como na vida); contra o escuro e com poste atrás, brilha.
  Risco gaussiano na largura. `Drop Light`, `Drop Opacity`.
- **Goteiras de tempestade** (pedido: "as goteiras eram apenas uns pontos brancos caindo; quero goteiras realistas tipo as
  de beiras de casa numa tempestade, caindo como um mini rio"): com chuva forte (de 12 a 60 mm/h; `water[0].x`) um id de
  gota em 64 vira um **véu d'água** preso a uma beirada com água (`find_edge`, a mesma busca das goteiras) por 20 s,
  entrando e saindo suave: uma fita no plano vertical ao longo da beirada (6-28 cm pela água da célula e a vazão), saindo
  em arco e afinando ao acelerar; `rain.frag kind 4` quebra o véu em filetes de 2,2 cm que se separam mais embaixo, com
  pulsos descendo na velocidade da queda (`sqrt(2 g z)`), transparente (o que está atrás passa), borda mais clara, luz
  do `drop_light` (postes acendem o véu). Três outros ids fazem coroas seguidas no pé. Chuva fraca: as goteiras de
  antes (contas que crescem e caem). Antes: a primeira versão era opaca e branca como um pano (alpha 0,75, véu inteiro
  até embaixo); agora 0,45 e só filetes abaixo do topo.
- **Tempestade**: `Rain Mist` engrossa a neblina de altura pela intensidade (0,29·R^0,67 por km: ~2 km de visão a 20
  mm/h, ~600 m a 100 mm/h; sem ator de neblina cria uma, `weather.c`).
- **Estilos** (`Shading Style`: Realista, Cinematográfico, Estilizado; `rain_shading_defaults` no `components.h`): cada
  um ajusta os 14 campos novos, que continuam editáveis (não tornam a Predefinição Personalizada). O Estilizado também
  muda as formas (borda dura das poças, tom chapado, riscos claros). A predefinição Estilizada usa o estilo Estilizado.
- **Água na lente** (`Lens Water`, desligada por padrão): pedido seguinte, "as gotas têm que escorrer na tela de forma
  realista, dependendo do ângulo (frente, lado, cima, baixo), ter inércia, poder mexer a câmera rápido para tirar a
  chuva; tudo controlado pelo dev". `src/lens_water.c`: até 160 gotas simuladas na CPU no plano da tela (unidades de
  altura da tela). Chegam ao ar livre pela intensidade, mais olhando contra a chuva (`WS.shelter` sob telhado: secam
  em `Lens Dry Time`). Força = gravidade projetada na tela pela orientação da câmera (`-right.z`, `up.z`: reto escorrem
  para baixo, inclinada de lado, olhando para o chão ou para o céu quase param) + inércia (aceleração da câmera em g e
  aceleração angular, ficam para trás da lente; giro faz "swing") + ar ao andar para frente (para as bordas). Aderência:
  segura enquanto a força < G·(r_ref/r)² (gotas maiores soltam; `Lens Stickiness`), atrito menor deslizando, caminho
  irregular, perde água e deixa gotinhas e rastro (8 pontos). Um giro/sacudida forte (`Lens Shake Off`) arranca as gotas
  (as grandes primeiro). Fusão pelo volume. Corte de câmera (salto > 5 m ou > 60°) não empurra. Empacotado por quadro
  em voo (`WS.lens_buf`, ~105 KB: gotas, segmentos de rastro, 32 × 18 ladrilhos com até 24 itens) e lido no
  `composite.frag lens_water` só pelo ladrilho do pixel: gota esticada na direção em que corre (cauda), imagem atrás
  refratada (`Lens Refraction`) e desfocada (`Lens Blur`, 6 amostras), borda sem corte duro; rastro como película.
  Campos: Lens Water, Lens Drop Size, Lens Gravity, Lens Stickiness, Lens Inertia, Lens Shake Off, Lens Airflow, Lens Dry
  Time, Lens Trail, Lens Blur, Lens Refraction (Detalhes e Set Rain Property). Autoteste (`--water-test`, `lens_selftest`):
  reto escorrem para baixo, olhando para o chão nenhuma escorre, inclinada 60° escorrem de lado, um giro limpa a lente,
  secam fora da chuva, empurrão para a direita desliza para a esquerda. Antes disso houve duas versões por hash (sem
  estado): o `rain_drops` das paredes em tela cheia (+0,26 ms, cara de janela) e uma leve (+0,06 ms), sem inércia.
  Custo: composite 0,21 → 0,29 ms com a lente cheia; CPU desprezível (160 gotas).
- `GpuFrame.water` 88 → 92 (`water[86..90]`). Campos novos no fim do `CRain` (níveis antigos abrem com o Realista e a
  lente desligada). Nós **Set Rain Shading Style** e **Set Rain Puddle Color**; tudo também por Set Rain Property.
  Artigo `rain-shading` no Assistente. Teste `rain_lens` (a cena `rain` com `REALENGINE_RAIN_LENS=1.5`); autoteste
  `water`: estilos, predefinições e campos por nome.
- **Medidas** (`--scene rain`, 2560×1417, Médio, RTX 3060, GPU mínima, a mesma câmera, 0.33 × agora): visão geral
  2,72 → 3,03 ms (passe principal 1,44 → 1,65); de perto 2,54 → 2,83 ms; com a lente 1,5: composite 0,21 → 0,29 ms.
  ~290 fps (meta 200).
- Falta (resolvido logo abaixo): gotas nos atores, anéis das goteiras, neve na lente.

## Marcas molhadas, gotas nos atores, anéis das goteiras e neve na lente (2026-11-15, "adiciona o que ficou de fora e
## quando algo molhado entrar em um lugar seco molhar o chão, onde o personagem toca o chão ou pelas goteiras pingando")
- **Marcas molhadas** (`Wet Marks`, `Wet Marks Dry Time` 120 s, `Actor Drips`): na chuva o mapa das pegadas na neve
  (512² × 4 cm em volta da câmera) guarda o molhado das marcas (`water[49].x` = 2, secando linear em `.w`). Os pés de
  cada Character (os mesmos passos das pegadas na neve) levam água: encharcados ao ar livre na chuva ou pelo molhado
  próprio do ator, 10% a menos por passo (~20 passos até secar); cada passo carimba a sola com essa força. Um ator
  molhado onde a chuva não cai nele (sob telhado, ou depois da chuva) pinga: `molhado² × 2 × Actor Drips` pingos/s (decaindo a 10% em ~20 s fora da chuva) em
  volta dele (força negativa no carimbo = respingo: mancha de 2-4 cm com gotinhas em volta, `water_sim.comp`), e os
  dois mais molhados perto da câmera têm os pingos vistos caindo (`water[92..95]`, `rain.vert`: conta caindo e coroa).
  `rain_marks` (`water.glsl`) lê com B-spline cúbico 4 × 4 e limiar com ruído (bilinear desenhava blocos de 4 cm) e
  molha o chão mesmo onde a exposição é zero. Medida: GPU mínima igual (só lido em chão com marcas por perto).
- **Gotas nos atores** (`rain_actor_drops`): gotas paradas no espaço da parte (só a rotação de M = R·S: as partes do
  esqueleto têm escala não uniforme e o espaço do objeto achatava as gotas em linhas), triplanar pelas duas direções
  em volta da normal dominante (o "lado" de uma parede é constante em volta de um cilindro e fazia faixas), 2-4 mm,
  borda escura (a refração tira luz) e película com a normal da gota. Andam com a parte.
- **Anéis das goteiras**: na poça, as 4 vizinhas da célula; uma beirada 30 cm acima com água faz dois trens de anéis no
  ponto onde as goteiras caem (mais rápidos na tempestade).
- **Neve na lente**: com neve chegam flocos (maiores, presos até virarem água, derretem em ~4 s em gotas menores); no
  composite o floco é gelo branco desfocado. Autoteste: nenhum floco escorre, todos derretem.
- "Cinematic" já era msgid do estilo do editor ("Cinemática"); o estilo da chuva virou "Cinematic Look"
  (Cinematográfico), e Set Rain Shading Style aceita "Cinematic".
- Teste `rain_marks` (`REALENGINE_WALKER_SHELTER=1`: o Walker encharca na chuva e entra sob o telhado a cada 16 s):
  exige pegadas e o ator encharcado; o log da medida diz `N wet actors (max X)`.
- Depuração registrada: algumas capturas mostravam o Walker seco. A tabela por entidade estava certa nos dois quadros em
  voo (29/29 em 255); pintando no shader (verde = molhado próprio, preto = 0, vermelho = sem) a captura "seca" era
  preta e com o chão ainda sem película: a cena mais nova no instante da captura (travamentos da carga limitam o dt a
  0,1 s). Não era a chuva.
- **Registradores** (`--shader-stats`): o porte direto do `rain_drops` do Tlender (duas camadas de gotas paradas, o
  caminho por ruído, as gotinhas no rastro, uma struct por gota) e a película em 5 floats levaram o `mesh.frag` de 96
  para 128 registradores e fizeram o `mesh_rt` derramar memória local (16 → 96 bytes). Agora: uma camada (`rain_bead`),
  caminho por senos, a melhor gota numa vec4; a película em 2 palavras (`g_rcp`: normal octaédrica snorm16 e peso/alfa
  em half) e o reflexo dela em 2 halfs (`ce_put`/`ce_get`). Medida na vista de perto (GPU mínima, 3 rodadas cada): com
  ou sem as gotas nos atores 2,94-3,00 ms, igual ao passo anterior dentro do ruído (±0,15 ms); 0.33: 2,57 ms.

## Bugs da 0.34: tela roxa piscando, goteiras sumindo e o véu na Delight (2026-11-15, relatados pelo usuário)
- **"A tela fica flickando em cores meio roxas" / "o céu fica piscando quando se movimenta na tempestade":** a película
  d'água (`g_rcp`) chegava a alfa GGX ~1,6e-4 sem anti-aliasing especular; o sol nas ondinhas de uma poça dava um pico
  além do que o half float do alvo guarda (Inf/NaN), e o TSR espalhava como manchas roxas/pretas que piscavam com o
  ângulo. Agora a película tem o piso do material (rugosidade 0,045) e o mesmo alargamento pela variação da normal no
  pixel (Kaplanyan-Hoffman, derivadas fora do branch), no `mesh.frag` logo depois do do material.
- **"As goteiras ficam sumindo e aparecendo ao andar ou mudar o ângulo da câmera":** a busca das beiradas (véus e contas)
  sorteava células em volta da posição da câmera: andar trocava cada goteira de beirada. Agora `drip_spot` (rain.vert):
  ladrilhos de 3 m fixos no mundo, 6 × 6 em volta da câmera, cada slot o ladrilho do seu resto mais perto (como a caixa
  das gotas), o lugar dentro dele pelo hash do ladrilho. E o véu, uma fita no plano da beirada, sumia visto de lado:
  vira para a câmera conforme fica de perfil.
- **"Ao sair da chuva com a Delight, embaixo dela começou uma goteira enorme bugada":** a primeira superfície da chuva é
  traçada no conjunto estático; um personagem nele (pele `INST_SKINNED` ou peça do esqueleto `INST_ROUND_OCCLUDER`)
  virava "telhado", e a borda do corpo uma beirada com véu de tempestade. Agora `first_surface` (water_sim.comp) passa
  por corpos (até 3): o topo deles conta como algo que se move (`mv.w`) e o raio segue até o chão.

## Goteiras sem sentido, pingos fortes dos personagens e tela piscando (2026-11-15, relato do usuário pela sessão do Tlender)
"Tá criando goteiras em locais que não faz sentido, e as goteiras dos personagens em locais fechados tão muito intensas,
e também tá flickando a tela."
- **Goteiras:** qualquer borda de 30 cm com a célula de cima molhada era beirada (topo de parede fina, poste, degrau,
  caixa). Agora `find_edge` (rain.vert; as contas pingando usam a mesma busca) exige um telhado atrás (a superfície
  continua duas células para trás na mesma altura) e água parada ou correndo para a borda; o véu de tempestade só com
  vazão de verdade (`s` pela profundidade e velocidade da água, sem o piso de 0,35). Os anéis das goteiras nas poças
  usam o mesmo teste.
- **Pingos dos personagens:** eram molhado² × 4 pingos/s enquanto o ator secava (8 min). Agora × 2 e decaindo com o
  tempo fora da chuva (`out_t`): ~2/s encharcado nos primeiros segundos, ~10% depois de ~20 s, como a roupa.
- **Tela piscando:** o caminho do reflexo (raio traçado, SSR, denoiser, o reflexo virado pela película) era escolhido
  pelo peso inteiro da película, que os respingos e gotas animam a cada quadro; um pixel trocava de caminho quando uma
  coroa cruzava 0,25. Agora `g_rcs` (só o molhado e as poças, estáveis) decide o caminho; os respingos e gotas só mudam a
  normal e o peso dentro dele.
- Teste: área `agua` 9/9 (com a GPU 35% ocupada por outro editor).

## Escoamento, quedas d'água, lente colorida, molhado escuro e pingos dos atores (2026-11-16, relato com 4 imagens)
"Os rios de goteiras tão sendo criados em locais sem sentido" (o usuário chama de goteira o escoamento), "com a resolução
adaptativa as quedas d'água ficam flickando", "o modelo ficou muito preto", "vários locais que deveriam ficar molhados por
serem abertos não ficam", "a Delight fica com um monte de quedas d'água embaixo, muito branco e feio", "a tela fica
flickando ao ir para um local sem chuva com a resolução adaptativa".
- **Escoamento nas paredes** (`rain_rivulets`): eram bandas de ruído em toda parede ao lado de chão molhado, na altura
  inteira, até no lado de trás do vento. Agora são fios: colunas de 7 cm, algumas levam um fio sinuoso que começa um
  pouco abaixo do topo e engrossa descendo, com pulsos e um rastro úmido; só onde a água chega à face: a chuva que ela
  encara (`facing` × chuva) e o que transborda do topo (`rain_outflow` da célula acima da parede para o lado da face).
  A parede é molhada pelo que a alcança (face do vento, 40 cm de respingos no pé), não pelo chão até em cima.
- **Goteiras e véus pela vazão real** (`rain_outflow`: a água rasa que sai da célula pela borda, em células de chuva):
  conta pingando com 1,2 células, véu com 4 (cheio com 20). Degrau, topo de parede fina e borda de caixa só levam a
  própria chuva; telhado plano empoça e pinga pouco; véus onde a água converge. Os anéis das poças usam o mesmo teste.
- **Sombra de chuva**: a inclinação do mapa limitada a 0,7 (era 1,5: com o vento da tempestade cada parede deixava uma
  sombra seca maior que ela). As gotas visíveis seguem com a inclinação delas.
- **Lente roxa e verde**: a lente vinha antes da aberração cromática e do sharpen, que releem a tela em volta; o verde
  refratado com o vermelho e o azul de fora. Agora a lente vem depois.
- **Molhado escuro**: o escurecimento ia a `base^2,5 × 0,65` nos materiais porosos; agora até ~`base^1,9 × 0,8`, como o
  Tlender.
- **Pingos dos atores**: 4 ids por ator (eram 8), contas menores e com metade da opacidade, saindo da barra/das mãos; a
  luz das gotas sob um telhado é um terço do céu e sem sol (`drop_light`: a gota refrata o cômodo); a conta (`kind 3`)
  usa essa luz (era o céu × 6: branca dentro de casa).
- **Resolução adaptativa**: as larguras mínimas das gotas em pixels eram para 1440 linhas; com a resolução menor a gota
  ficava abaixo de um pixel e cintilava (`g_pxs`). Os filetes do véu têm pelo menos ~2 pixels (`fwidth`).
- Medida: a GPU estava 44% ocupada pelo editor do usuário (a 0.36 publicada mediu igual, 126 fps na chuva): FPS a
  refazer com a GPU livre.
- (2026-11-16, "quando a Delight molhada sai para um local seco o escoamento na parte de baixo do modelo fica muito forte e
  feio, pode tirar ele") os pingos vistos caindo dos atores molhados foram retirados (`rain.vert` e as fontes
  `water[92..95]`); ficam as manchas dos pingos no chão e as pegadas molhadas.

## A chuva pela física real (2026-11-16, "pesquise como realmente as chuvas funcionam ... mais realista em física e visual")
**Pesquisa** (fontes: Marshall-Palmer 1948 pela forma N(D) = N0 e^(-ΛD), N0 = 8000 m⁻³ mm⁻¹, Λ = 4,1 R^-0,21 mm⁻¹;
velocidade terminal de Atlas, Srivastava e Sekhon 1973 sobre as medidas de Gunn e Kinzer 1949, v(D) = 9,65 − 10,3 e^(−0,6D)
m/s, válida de 0,1 a 7 mm; extinção por lidar β ≈ 0,16 R^0,74 /km e visibilidade V ≈ 11,6 R^-0,63 km; ondas em anel de
gota na água no regime capilar-gravidade, número de onda ~0,2 /mm (~3 cm); Garg e Nayar 2006: o risco da gota é a gota
borrada pelo obturador, quase transparente; Tatarchuk 2006 (ToyShop): camadas de riscos, ondulações nas poças, névoa).
Contas (R em mm/h):

| R | Λ /mm | gotas/m³ (≥0,5 mm) | mediana D0 | v(D0) | impactos ≥1 mm /m²/s | visibilidade | risco 1/60 s |
|---|---|---|---|---|---|---|---|
| 1 | 4,10 | 1951 (251) | 0,90 mm | 3,6 m/s | ~150 | 11,6 km | 6 cm |
| 10 | 2,53 | 3165 (894) | 1,45 mm | 5,3 m/s | ~1350 | 2,7 km | 9 cm |
| 50 | 1,80 | 4437 (1801) | 2,04 mm | 6,6 m/s | ~4800 | 1,0 km | 11 cm |
| 100 | 1,56 | 5132 (2354) | 2,35 mm | 7,1 m/s | ~7700 | 0,64 km | 12 cm |

**O que mudou** (antes → agora):
- Tamanho de cada gota: fixo da predefinição (±50 %) → sorteado do espectro de Marshall-Palmer da intensidade (0,5 a 6
  mm), escalado para a mediana ser o Drop Size; predefinições com a mediana real (Garoa 1,0, Fraca 1,3, Moderada 1,7,
  Forte 2,0, Tempestade 2,35, Monção 2,6 mm; padrão 1,7).
- Velocidade: reta pelo tamanho → a terminal de Atlas por gota: gotas pequenas caem devagar e inclinam muito no vento,
  as grandes caem retas e rápidas (profundidade e variedade). A velocidade média (respingos, direção) também por Atlas.
- Risco: 1/22 s → ~1/35 s (10-25 cm conforme a velocidade). Opacidade: 0,55-1 → a cobertura do obturador D/comprimento ×
  30 (0,08-0,85): gota pequena quase invisível, grande visível, chuva aparece pela quantidade e pela luz.
- Quantidade: linear na intensidade → pelo espectro, e^(−0,5Λ)/Λ (cresce ~ com a raiz: garoa com muitas gotas finas,
  tempestade com gotas maiores): 48 mil a 50 mm/h (o total por quadro na cena `rain`: 36 mil → 52 mil).
- Anéis nas poças: ~45 /m²/s de 20 cm → a taxa real de impactos de gotas ≥ 1 mm (1500 (R/10)^0,75 /m²/s), cada anel um
  pacote de ~3 cm de onda andando ~18 cm/s e sumindo em 0,45 s, maior pela gota: a poça "ferve" na tempestade. Mapa
  128² (1 cm), células de 8 cm em 2 camadas (256² a 0,5 cm custava ~0,1 ms nas malhas por cache).
- Visibilidade: a névoa da chuva já batia com as medidas (2,9 km a 10 mm/h, 0,62 km a 100).
- Tela piscando com personagem secando: `w_nmix` (mistura de normais nunca zero: peça de duas faces vista por trás) e a
  trava de NaN/Inf na saída do `mesh.frag`.
- Custo (cena `rain`, vista geral, GPU por passe, 0.37 → agora): malhas igual; água +0,03 ms; gotas +0,08 ms.
- Falta: espectro também nos respingos (coroa pelo número de Weber) e névoa de respingos rente ao chão na tempestade.

## Gotas caindo, goteiras e filetes transparentes (2026-11-17, plano 0.43 Q4a)
Pedido: "gotas e filetes brancos: devem refratar/transmitir como as gotas paradas nas superfícies".
- **Como:** `rain.frag drop_water` dá às gotas caindo (`kind 0`), às goteiras (`kind 3`) e aos véus/filetes das beiradas
  (`kind 4`) o shading das gotas paradas (`rain_glass`, a película d'água): o que está atrás **refratado** (a gota é uma
  lentezinha: a cena lida invertida a ~2 raios dela, pelo jacobiano do `i_uv` na tela), **Fresnel** (F0 0,02) refletindo
  o mundo que o `drop_light` já calculava (céu/chão, a sala debaixo de telhado) e o espalhamento das luzes para a frente
  (`drop_light` menos esse mundo) como um **brilho pequeno** fora do centro. Nada mais de branco chapado.
- **A cena atrás:** a cópia do quadro anterior (`ssr_col`, binding 82, a mesma do SSR e do vidro refrativo) lida no
  ponto reprojetado (`rain.vert o_rep`: `cloud_prev_vp` − `view_proj` no centro da gota). O render faz a cópia em todo
  quadro com chuva (`rain_now`, também com reflexos por raio) e avisa o shader em `water[85].y` (cópia válida e do
  tamanho do render). Sem cópia (MSAA, o primeiro quadro) ou no estilo Estilizado: o shading de antes.
  `REALENGINE_RAIN_FLAT=1` força o antigo (a medida do "antes"). Log: "Rain: the drops read the scene behind them".
- **Medidas** (RTX 3060, 2560×1417, Épico, `--scene rain`, mínimo da GPU, 2 rodadas): antes (main a3bd783) 2,194/2,212 ms;
  depois 2,161/2,201 ms; `REALENGINE_RAIN_FLAT=1` 2,189/2,198 ms: **custo ~0** (a cópia já existia com o SSR ligado).
- **Teste:** `rain_clear_drops` (área `agua`: a cena `rain` no Épico com validação e o log da cópia).
- Artigo `rain-clear-drops` no Assistente.

## Clima regional e molhado persistente (Q27.6, 2026-11-23: o "sol aqui, tempestade ali" do GTA 6)
- **Campo de clima** (`weather_field.c/.h`): até 8 tempestades (centro, raio, pico em mm/h, vida com subida de 15%,
  platô e queda nos últimos 30%) andando com o vento de cima (1,5 × o vento + 3 m/s, mesma direção). Chuva = soma de
  gaussianas; nuvens = a bigorna, mais larga (raio × 1,8); vento = o predominante + a frente de rajada para fora (até
  ~6 m/s numa forte). Sem estado por célula: a CPU (a câmera, as nuvens, o mar, os nós) e a GPU (pass 15) somam o mesmo.
  **Storminess** mantém 0..4 tempestades selvagens, nascidas 2-6 km a barlavento (as primeiras de um campo novo em
  qualquer lugar em volta, já no meio da vida), no máximo uma a cada 20 s; somem com a vida ou a 15 km.
- **No ator Rain**: **Regional Weather** e **Storminess** (no fim do `CRain`; níveis antigos abrem sem). Ligado, a cópia
  do Rain do quadro leva a chuva e o vento do campo onde está a câmera: gotas, respingos, névoa, som, lente e a janela
  simulada seguem; as nuvens ganham cobertura sob a tempestade (`weather.c`), o oceano ganha a rajada no espectro (pelo
  meio m/s, refeito só quando muda).
- **Mapa molhado persistente** (`water_sim.comp` pass 15, `WS.persist`): 256² células de 16 m (4 km) numa janela
  toroidal em volta da câmera, cada uma água (cm), molhado e chave. Dentro da janela simulada: a média de 16 lugares dela
  (os que estão prontos para o lugar). Fora: a chuva dali (as tempestades; sem o campo, a do Rain em todo lugar) × o
  ganho, a perda com ganho até o que as pequenas depressões seguram (0,35 × Micro Relief: as poças) e daí só a
  evaporação real (uma poça dura horas longe da câmera). Um lugar novo na janela simulada nasce do mapa (antes: Start
  Water), e o chão fora da janela mostra o molhado do mapa (o bairro vizinho molhado ao longe, o daqui seco).
  1 MB; endereço em `water[52]`, tamanho em `water[53]`, tempestades em `water[13..20]` e `water[54]`.
- **Nós**: Add Storm, Clear Storms, Get Rain At, Get Wind At, Get Cloudiness At.
- **Medidas** (RTX 3060, 1440p, cena `weather_regional`): o pass 15 custa **0,02 ms** de GPU (água 0,288 → 0,308 ms,
  `REALENGINE_NO_PERSIST=1` compara); CPU do campo desprezível (8 tempestades). Teste `weather_regional`: aqui 0,00 mm/h,
  na tempestade a 1,5 km 59,9 mm/h; o chão dela com 0,18 cm de água ao sair e 0,20 cm (molhado 1,0) **10 minutos de
  jogo depois** (`water_persist_skip(600)`); `weather_field` (o campo na CPU). A cena roda no editor (sem `--play`: o
  teste move a câmera do editor).
- Fica para depois: as gotas caindo e a névoa vistas de longe dentro da tempestade (hoje só onde a câmera está), o
  vento regional no Foliage e no cabelo, o mapa em espaço do mundo em vez do da inclinação da chuva (com vento forte e
  chão alto as poças guardadas andam um pouco).

## A Chuva sem travar o quadro (2026-11-25, bug-fixer)
- A chuva molha as superfícies nas variantes RE_EXT do `mesh.frag`; o 1º quadro com chuva criava o conjunto inteiro no
  quadro (`ext_pipelines_ensure`: 257 ms de um quadro de 300-450 ms; 6,5 s numa execução da varredura, com o cache frio).
  Agora o conjunto vai num thread (`ext_pipelines_request`, render.c, o padrão do `rt_alpha_job`): até ficar pronto
  (~200 ms) desenham as pipelines comuns e as superfícies molham alguns quadros depois. Vale também para Light Function,
  luz Force Toon e material com lóbulos extras. `render_pipelines_pending` espera por ele (as medidas).
- Teste `place_rain_hitch` (Adicionar > Chuva num Terceira Pessoa novo, nenhum quadro acima de 150 ms): antes 426 ms
  (FALHOU), depois 56-64 ms.
