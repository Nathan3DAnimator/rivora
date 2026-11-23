# VR e AR (2026-11-08, pedido do usuário)

Pedido: "suporte a AR e VR para headsets e Android; AR com giroscópio e rastreamento em tempo real, o mais responsivo
possível; color e light matching para o AR sair realista; reconstrução do ambiente para a sombra dos personagens; VR
sempre acima de 100 fps; o usuário com controle total; otimização e qualidade em primeiro lugar".

## VR (`src/xr.c`, `xr_openxr.h`, `xr.h`)
- **OpenXR sem SDK no build**: lê o runtime ativo (`HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime` ou `XR_RUNTIME_JSON`),
  carrega a DLL e negocia como o loader (`xrNegotiateLoaderRuntimeInterface`). `xr_openxr.h` declara a parte do OpenXR
  1.0 usada (tipos e estruturas da especificação). Testado: o runtime da Meta (Quest Link) negocia e cria a instância;
  sem headset conectado o jogo segue no monitor.
- **Vulkan pelo XR_KHR_vulkan_enable**: as extensões de instância e de dispositivo que o runtime pede vão para o `vkctx`
  (`g_vk_extra_*`) antes de criar o Vulkan; a GPU que ele usa (`g_vk_pick_gpu`).
- **Os dois olhos num quadro do motor** (Q25; antes cada olho era um quadro inteiro, com um present cada): o 1º olho num
  quadro sem present (`vkctx_begin_frame_offscreen`), o 2º com o espelho na janela (no Quest nenhum present). O que tem
  histórico de tela fica desligado no VR (TSR, nuvens de tela, desfoque de movimento) e o MSAA vai a 4×.
- **Espelho na janela**: os dois olhos lado a lado (`vkctx_mirror_eyes`).
- **Entrada**: ações OpenXR para Touch (Quest/Rift) e o controle simples: poses (grip, aim), gatilho, grip, analógico,
  A/B/X/Y, menu, clique do analógico, vibração. Nós: **Is VR Active**, **Get HMD Pose**, **Get Motion Controller Pose**,
  **Get Motion Controller Input**, **Play Haptic Effect**.
- **Modo simulado** (`--vr-sim`): sem headset, olhos de 1280 × 1400 a 6,4 cm, fov 100°, lado a lado na janela.
- Como usar: `Jogo.exe --vr` (ou `REALENGINE_VR=1`, ou `vr=1` no game.ini: Project Settings > VR > Iniciar em VR); `--vr-sim` para testar sem headset.

## Q25 — VR completo (2026-11-18, "total controle ... interações ... Quest standalone acima de 100 fps")
Assistente: `vr-quest` (pt/en/es). Testes (área `xr`): `vr_selftest` (`realengine --vr-test`), `vr_sim`, `vr_quest_sim`, `project_vr`.

### Projeto e editor
- **Project Settings > Plataformas > VR** (`ProjectVr vr` no `ProjectSettings`, chaves `vr_*` no .rproject): Iniciar em VR,
  PC VR / Meta Quest, taxa (72/90/120, `XR_FB_display_refresh_rate`), espaço (Chão = STAGE, Sentado = LOCAL + altura),
  locomoção (Teleporte / Suave / os dois), giro (passos / suave / nenhum), ângulo, velocidades, alcance, vinheta de conforto,
  mãos, rastreamento das mãos, laser, Arrastar para Andar, Qualidade em VR (Baixo..Épico, Quest), foveação (0-3, dinâmica),
  SpaceWarp, escala de renderização.
- **game.ini** lido de verdade: `vr`, `vr_refresh`, `vr_tracking`, `vr_quality`, `vr_foveation`, `vr_foveation_dynamic`,
  `vr_spacewarp`, `vr_render_scale` (`xr_config_key`); `--vr-ini chave=valor` numa execução; `--project <.rproject>` roda o
  jogo direto da pasta do projeto.
- **Play > VR Preview** (`editor_vr_preview`): o nível como está (sem salvar também) em `Saved/PIE/VR_Preview.rlevel` com a luz
  assada, aberto pelo `realengine_game.exe --vr --project`: processo próprio (as extensões Vulkan do runtime e o ritmo do
  headset não tocam o editor).
- **Compilar > Empacotar para Meta Quest (VR)** (`editor_package_quest`, `--package-quest <pasta>`): o empacotador Android com
  `-Quest [-HandTracking]`: o loader OpenXR da Khronos (Apache-2.0, baixado uma vez do Maven Central para
  `%APPDATA%\RealEngine\android\openxr\<versão>`) em `lib/arm64-v8a`, o manifesto VR da Meta (headtracking obrigatório,
  categorias `com.oculus.intent.category.VR` e `IMMERSIVE_HMD`, `supportedDevices` quest2|questpro|quest3|quest3s, focus aware,
  as `queries` do runtime broker), paisagem, `vr=1`. No Android o `xr.c` abre o loader por `dlopen`, `xrInitializeLoaderKHR`
  com a VM e a atividade, `XR_KHR_android_create_instance`. **Ainda não instalado no Quest** (o adb não ficou estável: ver Falta).
- **Template Realidade Virtual** (`--template 9`): mesa, cubos Pegáveis com física, bola Encaixar na Mão, Encaixe de Pegar,
  porta e alavanca, plataforma, `BP_VRPawn` (Event On VR Teleported -> Play Haptic Effect).

### Pawn VR (`src/vr_pawn.c/.h`)
- O pawn possuído em VR: o espaço de jogo segue os pés dele (`off` = onde ele está no espaço; o andar no quarto não o move),
  o yaw é o dele. Teleporte: arco balístico pela mão (alcance a 45°), válido só caindo num chão (traço para baixo), a cabeça
  pousa no ponto, a cápsula embaixo dela. Giro por passos/suave em volta da cabeça (cabeça e pawn ficam onde estão). Suave:
  `CCharacter.input` na direção da cabeça (colisão, gravidade e escadas do Character). Vinheta de conforto pelo
  `terror[2][1]` do composite (sem shader novo).
- Mãos: caixas por osso (controle: dedos fechando com gatilho e grip; mão rastreada: as juntas). Laser (pose de mira; mão
  rastreada: do peito pela palma) até o painel de menu, um Interagível ou o que o traço acha. O Interactor do jogador olha
  pelo laser e o gatilho usa (`interaction_vr_pointer`). Ponta do indicador dentro da caixa de um Interagível o usa.
- Pegar (bits `IA_GRAB`, `IA_GRAB_SNAP`, `IA_GRAB_SOCKET` no `CInteractable.flags`): grip/punho pega o mais perto a 10 cm,
  segue a mão (onde foi pego ou encaixado na mão), solto: arremessado com a maior velocidade de 2 quadros nos últimos 80 ms
  (a mão freia ao soltar: a média jogava fraco) ou encaixado num Encaixe a 20 cm (Tag Exigida). Eventos On VR Grabbed /
  Released (Hand, Velocity, Socket) / Teleported; nós VR Teleport, Set VR Locomotion, Set VR Comfort Vignette, VR Grab,
  VR Release, Get VR Held Actor, Get VR Laser Hit, Get VR Hand Tracking, Set VR Refresh Rate.
- Arrastar para Andar: mão fechada no ar segura o mundo (as duas: a média), soltou: deslize que freia em ~0,25 s; as paredes
  param (traço à altura da cintura). Um salto da mão > 15 cm num quadro é falha do rastreamento: reancorado, nada de pulo.
- Painel de menu: com um widget pedindo o mouse, a UI vai para uma camada quad do OpenXR 1,2 m à frente (1280 × 800); o laser
  é o mouse (`ImGuiIO_AddMousePosEvent`), o gatilho o botão.

### Controle total (todas as opções em Project Settings > VR e como console variables `vr.*`)
- O pawn lê `project()->vr` a cada quadro (`sync_settings`): os nós (Set VR Locomotion, Set VR Comfort Vignette) e
  `Set Console Variable vr.*` mudam durante o jogo (28 variáveis: vr.Locomotion, vr.SnapAngle, vr.PhysicalHands,
  vr.HeadCollision, vr.HeadroomFps...).
- **Mãos físicas** (opção do dev, `vr.PhysicalHands`): a mão desenhada vai de onde estava até a real numa esfera de 3 cm e
  para no nível fixo (paredes, chão, mesas); depois a palma e as 5 pontas dos dedos contra o nível e as caixas do que se
  move perto (corpos com física, Interagíveis; os dedos só entram num Pegável fechando), a mão recuada pelo ponto mais fundo
  (3 vezes): os dedos entravam nos cubos pelas frestas. **Colidir com Objetos** (`vr.HandObjects`, nó Set VR Hand
  Collision): desligado, só o nível para a mão. **Empurrar** (`vr.HandPush`): cada ponto real da mão (o desenhado menos o
  deslocamento) dentro da caixa de um corpo com física (1 cm de pele) o tira pela face mais perto na mesma medida
  (velocidade = profundidade / dt × 0,8, até 5 m/s, só o que falta), em qualquer velocidade: só a velocidade (> 30 cm/s)
  empurrava e a mão aberta pousada devagar não fazia nada. Pontos opostos se anulam (pinça e punho em volta não empurram) e
  a mão fechando num Pegável não o empurra; a mão rápida a 3,5 cm ainda dá a velocidade dela (nunca mais que ela: um cubo
  subiu 1,5 km pela realimentação). **O que a mão segura**: `TRANSFORM_NO_COLLISION` enquanto segurado (empurrava a
  cápsula do jogador: o "teleporte para o nada"), CCD ao soltar (os cubos atravessavam o chão).
- **Mãos reais** (rastreadas, sem controle): a pose pela palma, sempre pelas juntas (o runtime emula um controle com grip 0).
  Curvatura de cada dedo = 1 − (ponta-base / comprimento) normalizada (0,86 reto .. 0,46 punho): a distância das pontas à
  palma lia a mão aberta relaxada como meio fechada. Pinça pelo tamanho do indicador. Toque no indicador: a ponta na metade
  de cima dele (falange do meio à ponta), 1 cm conta e 2 cm não; médio/anelar/mínimo só curvados (> 0,35): a mão aberta com
  os dedos juntos (o médio ao lado do indicador, o polegar na base) segurava as coisas. Formas: Aberta, Punho, Apontar,
  Pinça, Joinha, Paz. **Gesto de Pegar** e **Gesto de Arrastar** escolhidos pelo dev: Punho, Pinça, Qualquer Dedo no
  Indicador, Punho ou Qualquer Dedo (padrão de pegar). Nós Get VR Finger Curls, Get VR Hand Gesture, Get VR Finger Touch.
- **Arrasto** pela mão real (a física presa no chão que ela agarra não puxava nada), descendo da mesa (só o vazio para:
  chão até 15 m abaixo). **Rede de segurança**: caiu 5 m abaixo do último chão ou 6 s caindo: volta ao último lugar firme;
  outro resgate em 3 s vai a um mais antigo (8 guardados a 1 m), por fim ao início (o loop de resgates do teste).
- **Espelho para gravar/transmitir** (`vr.MirrorMode`): um olho recortado e ampliado na janela (padrão; sem foveação nele: o
  "círculo de vidro"), os dois olhos, ou a Câmera do Espectador (cabeça suavizada, FOV próprio).
- **Cabeça na parede** (do pawn, à altura da cabeça, até a cabeça, esfera de 8 cm): Escurecer (padrão: a vista vai ao preto
  pelo canal Dip to Black do composite, 15 cm dentro = preto) ou Empurrar (o espaço de jogo recua).
- **Corpo do pawn escondido** (as malhas dele e dos componentes dele, de novo a cada 30 quadros: as partes do Skeletal Mesh
  nascem depois); o template perde o manequim (o componente "Mesh" do BP_VRPawn).
- **Malha da mão** por mão (Static Mesh do Content, escala, rotação) ou, desde 2026-11-20, uma Skeletal Mesh com os
  dedos movidos (seção "Mão base" abaixo).
- Mão dominante (canhotos: analógicos e laser trocados), direção do andar (cabeça / mão), piscar no teleporte e no giro,
  alcance para pegar, pegar a distância (grip no Pegável que o laser aponta), força do arremesso, vibração, comprimento do
  laser, distância e largura do menu, MSAA do VR, **Meta de Folga** (padrão 150 fps: a foveação dinâmica mira 6,7 ms para
  os dois olhos mesmo a 120 Hz; a medida imprime "fps of GPU: the headroom").

### Desempenho
- **Trabalho compartilhado**: o 2º olho (`RenderView.stereo_second`) não refaz água, tecido, fogo, sondas, cache de
  superfície, oclusores, passo do bake e mistura dos lightmaps (antes rodavam duas vezes por quadro: as simulações andavam
  em dobro no VR). `REALENGINE_VR_SHARE=0` desliga (a medida).
- **Foveação fixa** pelo VRS que já existia: variante `vrs_fov.comp` (`-DRE_VRS_FOVEATED`, nada lido: com MSAA 4×), anéis
  1×1 / 2×1-1×2 / 2×2 pelo nível; dinâmica: sobe acima de 90% do período do display, desce depois de 1 s abaixo de 70%.
- **Preset Quest** (`vr_quality_apply`): Médio + nada por raio, sem cache de superfície, sondas (volume assado) e lightmaps,
  sem GTAO/SSR/sombras de contato/SSS, neblina e nuvens de tela fora, VSM 5 × 1024 × 4 amostras, LOD 3 px.
- **FOV assimétrico**: o renderer desenha simétrico (o maior lado), maior que o recomendado (swapchain com 25%/15% de folga),
  e entrega ao compositor só o recorte do FOV real de cada olho com esse FOV: os olhos do Quest 3S são −52/+45 e −45/+52°;
  com o simétrico informado o primeiro teste viu duplo. O recorte fica com a resolução recomendada (2079 × 2100).
- **Multiview (VK_KHR_multiview) não feito**: o renderer tem dezenas de passes de tela por olho; o ganho que dava para ter sem
  reescrevê-los veio do trabalho compartilhado. **SpaceWarp não feito**: a opção existe e o runtime é consultado
  (`XR_FB_space_warp`), mas os vetores de movimento e a profundidade não são enviados.

### Medidas (RTX 3060)
| Cena | Resultado |
|---|---|
| `vr_sim` (jogo de teste, 2 × 1280 × 1400, MSAA 4×) | 463 pares/s, CPU 0,05 ms (antes: 122 pares/s, um present por olho) |
| `house` simulado, trabalho compartilhado (3 pares alternados) | 287-290 pares/s, GPU do par 3,37-3,39 ms; sem: 277-278, 3,52-3,54 ms |
| `house` simulado, Épico × Quest × Quest + foveação Alta | GPU do par 4,72-4,75 / 4,27-4,29 / 2,78-2,80 ms (209 / 229 / 347 pares/s) |
| **Quest 3S pelo Link**, `house`, 2080 × 2096 por olho, 72 Hz | foveação dinâmica: 71 quadros/s, GPU do par 7,9 ms; sem foveação: 55, 12,4 ms |
| **Quest 3S pelo Link**, template VR | 72 quadros/s travados, GPU do par 6,4 ms |
| autoteste `vr_selftest` | 0 falhas; os dois ticks do pawn 0,002-0,004 ms por quadro (com a mão inteira e o empurrão pela penetração: 0,0026 ms) |
| teste jogado `vr_room_played` (template empacotado, headset simulado) | pega, segura (a cápsula parada), arremessa 165 cm, cai 16 cm; arrasta 57 cm (40 puxados + o deslize); 192 pares/s |
- O Link só ofereceu 72 Hz (a taxa vem do app Meta Horizon Link no PC). A camada de validação com o headset acusa 11 erros do
  próprio runtime da Meta (um shader dele pede `shaderStorageImageMultisample`; command buffers dele reenviados): os nossos
  80 pipelines já estavam criados antes, e cada handle é outro.

## Mão base e mãos com esqueleto (2026-11-20, "baixar mão base em FBX, com os ossos e as animações, para trocar depois")
- **O pedido**: uma mão para passar a um modelador e a um rigger, e a engine movendo a mão que voltar.
- **Project Settings > VR > Exportar Mão Base (FBX)...** (`vr_hand.c`, gravador `fbx_write.c`): grava numa pasta
  `RealEngine_BaseHand_L/_R.fbx` e um leia-me (pt/en). Cada FBX tem:
  - a malha com pele (palma com eminência tenar, dedos em tubos com ponta arredondada, polegar, toco do antebraço;
    2.220 vértices, 4.368 triângulos), com pesos de até 4 ossos misturados 0,5 cm em cada junta;
  - os 26 ossos do OpenXR (`Wrist_R`, `IndexProximal_R`...), em cm, Z para cima;
  - 7 poses: Open, Relaxed, Fist, Grip, Point, Pinch (o polegar mirado até a ponta do indicador), ThumbsUp. Cada uma é
    uma AnimationStack `<Pose>_<L|R>` (do quadro 0 aberto ao 10 na pose).
- **Usar a Mão Base**: exporta para `<projeto>/Saved/BaseHand`, importa pelo importador FBX da engine em
  `/Content/VR/Hands` (`SK_BaseHand_L/R.rskm`, `A_<Pose>_<L|R>`) e coloca as duas como mãos do VR.
- **Em jogo** (`vr_pawn.c` `sk_hand_*`): uma Skeletal Mesh na Malha da Mão vira um ator com o mesh. `vrh_rig_build`
  acha os ossos pelos nomes das juntas do OpenXR (em qualquer parte do nome, sem maiúsculas nem separadores; "Pinky"
  vale por Little). A pose entra por `anim_pose_override` (anim.c: o pawn escreve antes do `anim_update`, as jobs leem).
  - **Rastreadas**: a mão fica no pulso e nos nós dos dedos reais; cada osso gira (swing) para a próxima junta.
  - **Controle**: no punho do controle como a Static Mesh. Gatilho = indicador, grip = os outros, botões = polegar, entre
    as animações Open e Fist da própria mão (achadas pelo nome); sem elas, pelos eixos dos ossos (normal da palma ×
    direção do dedo).
  - A pele do ator sem colisão também fica sem colisão: antes ela virava colisor estático e o cubo arremessado batia nela.
- **Compatibilidade conferida** (scripts na pasta de notas, não na suíte):
  - **Blender 5.1**: 2 armaturas de 26 ossos, 20 grupos de pesos, as 7 ações por mão; o render das 6 poses está certo.
  - **Maya 2023** (FBX SDK): 26 juntas, `skinCluster` com 26 influências, os 7 takes, 78 curvas de rotação, o dedo a
    (0, 100, 0) no quadro 10.
  - O que o FBX SDK exige e o Blender também escreve: o `AnimationLayer` com um bloco aninhado vazio (sem ele o Maya
    descarta a camada e todas as curvas) e Euler perto de zero (100° em Y, não 180/80/180: a interpolação desde o
    quadro 0 girava o dedo).
- **Testes**:
  - `vr_base_hand`: exporta, lê de volta pelo `fbx_read`, importa e confere:
    - o bind a 0,02 cm das juntas da mão base;
    - os clipes Fist/Point/Pinch a 0,015 cm das poses, e o meio do Fist a 50°;
    - no Pinch, as pontas do polegar e do indicador a 1,29 cm;
    - o rastreado a 0,35 cm (com as juntas giradas e movidas no mundo);
    - o punho do controle a 0,012 cm pelos clipes e a 1,3 cm pelos eixos dos ossos.
  - `vr_hand_package` + `vr_hand_played`: o projeto empacotado com a mão base, jogado no VR simulado com o roteiro de
    pegar/arremessar/arrastar (155 cm arremessados, igual às caixas).
- **Medidas**:
  - solução rastreada + dedos pelo controle: 0,0015 ms por mão (CPU);
  - exportar e importar as duas mãos: ~100 ms;
  - VR simulado: 348 contra 321 fps com as caixas (ruído; a pele vai na GPU como a de qualquer personagem).
- **Falta**: a orientação da mão no punho do controle segue a das caixas (palma para baixo, ao longo do controle) e só
  foi vista no simulado; no headset pode pedir um ajuste em Rotação da Malha da Mão.

## AR (`src/ar.c`, `ar.h`)
- **Color e light matching** (qualquer foto/quadro de câmera): exposição pela média logarítmica (EV100), balanço de
  branco por mundo cinza (temperatura de cor por McCamy, tint), luz principal (região mais clara → direção na vista e
  força), ambiente em 9 harmônicos esféricos; aplicado ao sol, à luz do céu e à exposição, suavizado no tempo.
  No PC: `Jogo.exe --ar-image foto.png`.
- **Giroscópio (Android)**: o sensor de vetor de rotação (giroscópio + acelerômetro + bússola, 200 Hz) gira a câmera
  (`--ar`): AR de 3 graus de liberdade.

## ARCore (2026-11-08): 6-DoF, planos, a imagem da câmera, a sombra no chão real
- **Sem SDK no repositório**: `src/ar_arcore.c` carrega `libarcore_sdk_c.so` por `dlopen` (ponteiros com `__typeof__` das
  declarações do `arcore_c_api.h`). O cabeçalho é baixado pelo CMake no build do Android (`build-android/arcore`, tag
  v1.57.0); o AAR (`core-1.57.0.aar`) pelo `android/package_apk.ps1 -AR`, uma vez, em `%APPDATA%\RealEngine\android\arcore`.
  A licença deles é a "ARCore Additional Terms of Service": nada disso entra no git.
- **Empacotar** (Project Settings > Android > Realidade Aumentada, `android_ar` no `.rproject`): os dois `.so` do AAR em
  `lib/arm64-v8a`, o `classes.jar` dexado como `classes2.dex` (multidex nativo), no manifesto a permissão CAMERA,
  `android.hardware.camera.ar` não obrigatório, `<queries>` do pacote da ARCore e da Play Store, `com.google.ar.core =
  optional`. **Deixado de fora**: a tela de instalação da ARCore (`InstallActivity` + os recursos do AAR, que pediriam
  compilar os recursos dele e gerar o `R`); sem o Google Play Services para AR o jogo roda sem AR (`arcore_open_store`
  abre a página da loja).
- **Sessão**: `ArCoreApk_checkAvailability`, permissão da câmera pelo Java (`hasCameraPermission`/`requestCameraPermission`,
  tentado de novo a cada segundo), planos horizontais e verticais, luz Environmental HDR (senão a básica), câmera mais
  recente, foco automático, textura `EXPOSE_HARDWARE_BUFFER` (sem textura OpenGL: a imagem vem como imagem de CPU). Pausa
  e retoma com a atividade (`re_android_set_pause_callback`); a rotação da tela a cada 60 quadros (`displayRotation`).
- **Eixos**: ARCore (m, Y para cima, −Z para frente, destro) → motor (cm, X frente, Y direita, Z cima): x = −z, y = x,
  z = y (um reflexo). A pose da câmera orientada à tela dá posição e base; o FOV horizontal sai da projeção (`1/P[0]`).
- **Imagem da câmera**: `ArFrame_acquireCameraImage` (YUV_420_888) copiada para Y e UV intercalado; `render_ar_frame`
  copia só quando chega uma nova (`img_serial`), `ar_record` envia por staging às imagens R8/RG8 (ligações 63 e 64) e o
  `sky.frag` a desenha no lugar do céu: YUV → RGB (BT.601), sRGB → linear e **a exposição e o ACES do composite
  invertidos** (a tela mostra as cores da câmera). Os cantos da tela na imagem: `ArFrame_transformCoordinates2d`.
- **Luz**: a estimativa da imagem (`ar_estimate` numa cópia 96 × 72, a cada 10 quadros: exposição, balanço de branco,
  ambiente) e a direção/força da luz principal da ARCore (HDR) quando válida; aplicadas ao sol, ao céu e à exposição.
- **Planos**: a cada 8 quadros; os que olham para cima viram triângulos (leque do contorno) para o **shadow catcher**
  (`ar_plane.vert/.frag`, mistura pré-multiplicada sobre a imagem: perde a parte do sol onde a sombra virtual cai e a do
  céu pela AO de contato dos atores móveis). Todos viram atores **AR Plane** (Box Collision que bloqueia, 4 cm sob a
  superfície): os personagens andam no chão real (`bp_boxes_changed`).
- **Nós**: Is AR Tracking, AR Line Trace (`ArFrame_hitTest`), Get AR Plane Count.
- **No PC** (`--ar-image foto --ar-sim`): a mesma passagem pelo renderer com a foto e um chão de 8 × 8 m na origem; os
  planos de chão do nível são escondidos e o sol fica o do nível (as sombras conferidas). Testes `arcore` (sessão simulada:
  piso, triângulos, toque, YUV) e `ar_sim` (o jogo empacotado com validação, ≥ 200 fps).

## Medidas (RTX 3060)
| Cena | Resultado |
|---|---|
| `vr_sim` (o jogo de teste, 2 × 1280 × 1400, MSAA 4×) | 244 quadros de olho/s = 122 pares/s |
| autoteste `ar` | quente 3.214 K, azulado > 6.500 K, luz no canto certo |
| `ar_sim` (jogo de teste, 2560 × 1440, foto 2K atrás) | 559-563 fps, CPU 0,45 ms (sem AR: 491-494 fps, CPU 0,39-0,40 ms; a foto substitui céu e nuvens) |
| cópia da imagem a cada quadro (antes do `img_serial`) | CPU 4,3 ms com a foto 2K: agora só quando chega uma nova (a câmera manda 30/s, 640 × 480 por padrão) |

## Oclusão pela profundidade real (2026-11-08, "vamos fazer o que falta")
- A ARCore com `AR_DEPTH_MODE_AUTOMATIC` onde o aparelho tem (`ArSession_isDepthModeSupported`); a cada quadro
  `ArFrame_acquireDepthImage16Bits` (mm, ~160 × 120), copiada só quando o timestamp muda; os cantos da tela nela pelas
  coordenadas da **textura** da câmera (`AR_COORDINATES_2D_TEXTURE_NORMALIZED`, como as amostras do Google).
- O renderer (`render_ar_depth`): imagem R16 (binding 65), enviada só quando chega outra. No lugar do céu, com a
  profundidade ligada, o `ar_occlude.frag` desenha a imagem da câmera **e escreve a profundidade real** (`near / d`,
  Z invertido), teste `>=`: onde o real está mais perto que o virtual, a imagem cobre o virtual. A profundidade fica no
  buffer: o plano de sombra do chão, a chuva e as partículas atrás de algo real também somem. Empurrada 3% + 3 cm para
  trás: o chão real sob um objeto (tão perto quanto a base dele) nunca o corta, nem o próprio plano de sombra.
  Bilinear só entre os texels conhecidos (um 0 desconhecido misturado leria "muito perto").
- O código da imagem da câmera saiu do `sky.frag` para `shaders/ar.glsl` (o céu e a oclusão usam o mesmo).
- Simulado (PC): a profundidade do chão de 8 × 8 m ao longo de cada raio, 160 px na largura da foto, refeita só quando a
  câmera mexe; `REALENGINE_AR_SIM_FLOOR=<cm>` sobe o chão (a base dos objetos some: a conferência visual);
  `REALENGINE_NO_AR_OCCLUSION=1` desliga. Teste `arcore`: a profundidade no meio (2,5 m pelo eixo) e mais longe no topo.
- Project Settings > Android > Oclusão pelos Objetos Reais (padrão ligada) → `-ARNoOcclusion` no empacotador →
  `ar_occlusion=0` no `game.ini`.
- **Exposição do AR simulado**: antes ficava na automática com o sol do nível, e a imagem (desenhada para mostrar as cores
  da câmera em qualquer exposição) só realimentava a medição: com uma foto escura a exposição ia ao máximo e os objetos
  ficavam brancos. Agora o simulado casa a luz como o celular (`ar_apply`: EV da foto, sol e céu dela).
- `GameConfig` zerado antes de ler o `game.ini` (os campos que ele não tem ficavam com lixo da pilha).
- Medida (RTX 3060, 2560 × 1440, `ar_sim`): GPU 1,708 ms sem oclusão, 1,717 ms com (no ruído); CPU igual. No S23 ainda a
  medir (um passe de tela cheia como o do céu, com 4 leituras da profundidade).

## Toque e template de AR (2026-11-08)
- Nós **Event Touch** (Pressed / Released, Location em px da vista, Finger Index; como o InputTouch da Unreal) e **Get
  Mouse Position** (puro). `bp_touch_update` por quadro (jogo: a janela; editor: o viewport do Play):
  `platform_touch_points`, senão o mouse como dedo 0 enquanto o botão esquerdo está apertado. Um dedo novo espera um
  quadro: se um widget de toque o pegou (`ui_touch_held`), nunca dispara. A receita do Assistente usava nós que não
  existiam (Event Touch Pressed, Touch X/Y): agora usa estes.
- `arcore_hit` simulado usa o tamanho real da tela (supunha 1920 × 1080).
- Template **Realidade Aumentada** (`--template 8`): chão (escondido no AR), `BP_ARObject` (caixa de 30 cm),
  `BP_ARPlacer` (Event Touch → Break Vector → AR Line Trace → Branch → Spawn a +15 cm), Android com AR e oclusão.
  Teste `project_ar` (o nível feito, as classes compilam e jogam).

## Q25b — Realidade mista no Quest 3S (2026-11-18, "mexer no passthrough/AR, suporte e controle ao dev, reconstrução do ambiente")
Código: `src/xr_mr.c/.h` (OpenXR + o quarto + atores + o que vai ao renderer + autoteste), `src/xr_openxr_mr.h` (as
extensões da Meta declaradas da especificação: valores e layouts conferidos no `openxr.h` da Khronos de 2026),
`src/xr_internal.h` (a sessão do `xr.c` para o `xr_mr.c` e os ganchos), `shaders/mr.glsl` + `composite.frag`/`sky.frag`.
Assistente: `mixed-reality` (pt/en/es). Testes (área `xr`): `mr_selftest` (`realengine --mr-test`), `mr_sim`.

### Ligar (Project Settings > VR > Realidade Mista; `ProjectVr.mr_*`, chaves `mr_*` no .rproject)
- **Realidade Mista (Passthrough)** é a chave mestra: só com ela as extensões são pedidas ao runtime (`xr_mr_pick_extensions`:
  XR_FB_passthrough, _spatial_entity, _query, _container, XR_FB_scene, _scene_capture, XR_META_spatial_entity_mesh,
  XR_META_environment_depth); desligada o jogo é VR puro (nada muda). `REALENGINE_MR=1` força.
- Tudo o mais também como console variable (`vr.MixedReality`, `vr.Passthrough`, `vr.PassthroughBackground/Opacity/Style/
  Brightness/Contrast/Saturation/Blend`, `vr.MROcclusion/OcclusionSoftness/HandRemoval/Shadows/Light/Exposure/RoomActors/
  RoomMesh/AlignRoom/RealWindows/HideFloor/DepthFlipY`) e lido a cada quadro: os nós e `Set Console Variable` mudam em jogo.
- **Link (PC)**: no app Meta Horizon Link, Configurações > Beta > Developer Runtime Features > Passthrough over Meta Quest Link
  e Spatial Data over Meta Quest Link. **Quest sozinho**: `package_apk.ps1 -Quest -MixedReality` (o editor passa quando a chave
  está ligada): `com.oculus.feature.PASSTHROUGH` (não obrigatório: um Quest 2 roda como VR) e a permissão
  `com.oculus.permission.USE_SCENE`, pedida em jogo pelo `GameActivity.requestScenePermission` antes da consulta do quarto.

### Passthrough (XR_FB_passthrough; sem ele, o ambiente ALPHA_BLEND quando o runtime o enumera)
- A camada de passthrough (`XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB`) vai **embaixo** da projeção; a projeção ganha
  `BLEND_TEXTURE_SOURCE_ALPHA` e o **alfa da imagem do olho** (pré-multiplicado) decide onde o real aparece. Criados pausados
  no início da sessão, ligados/pausados conforme o que o jogo pede (`passthrough_apply`, cada quadro).
- **O alfa** (`composite.frag mr_out`, a profundidade do passe principal de uma amostra, agora resolvida também com MR):
  0 onde nada virtual foi desenhado com **Fundo = O Quarto** (o `sky.frag` desenha preto no lugar do céu), 0 sob uma
  **Passthrough Window**, a oclusão pelo real (abaixo); o que é desenhado sem profundidade sobre o fundo vazio (faíscas,
  brilhos, vidro) fica somado ao real (o resto de grão/neblina abaixo de 0,02 cortado). O fade (Dip to Black, a cabeça na
  parede) cobre o real também.
- **Estilos** pelo próprio compositor (`xrPassthroughLayerSetStyleFB`, só quando muda): opacidade (`textureOpacityFactor`),
  cor das bordas, Cor / Preto e Branco (mapa mono→mono identidade) / Uma Cor (mono→RGBA: a cor × o brilho) / Ajustado
  (brilho −100..100, contraste, saturação). **Opacidade do Virtual** (Set Passthrough Blend): o virtual some no real.
- **Passthrough Windows**: os limites de um ator (Set Passthrough Window, 16 no máximo) e, com **Janelas e Portas Reais**,
  as janelas/portas do Space Setup (uma caixa de 5 cm atrás a 55 cm na frente da parede): o virtual dentro e atrás some.
  Feito no composite (respeita a profundidade: o que está na frente da janela fica), sem a camada projetada da FB.

### O quarto (reconstrução do ambiente)
- **Consulta** (XR_FB_spatial_entity_query): ROOM_LAYOUT → o container de cada quarto (uuids) → as entidades por uuid
  (sem container: as que têm SEMANTIC_LABELS) → LOCATABLE (e TRIANGLE_MESH) ligados quando não estão → lidas: rótulos
  (pedidos os novos: DESK vira TABLE, INVISIBLE_WALL_FACE), pose, retângulo 2D + contorno, caixa 3D, a malha do GLOBAL_MESH.
  Sem quarto: **Pedir o Mapeamento** abre o Space Setup (`xrRequestSceneCaptureFB`), e ao terminar a consulta volta.
  As âncoras são relocalizadas a cada 2 s (o espaço de jogo recentrado: o quarto vai junto).
- **Atores**: cada plano e móvel vira "MR Floor", "MR Ceiling", "MR Wall 2", "MR Table", "MR Couch"... um cubo com
  `TRANSFORM_NO_RENDER` (flag nova: colide e nunca é desenhado; o renderer o pula na lista de instâncias): estático da
  física (o OBB pela rotação), caixa dos Characters e dos traços. Os planos ganham 4 cm atrás da face. Seguem o pawn VR
  (teleporte, giro: o quarto real anda com o espaço de jogo no mundo).
- **Alinhar o Nível ao Quarto**: ao carregar, o espaço de jogo gira em volta da cabeça (`vr_pawn_turn_play_space`) para a
  parede mais longa ficar no X ou no Y do mundo: as caixas dos Characters (AABB) ficam exatas e o nível casa com o quarto.
- **Line Trace Real World** (`mr_line_trace`): planos, volumes e os triângulos da malha do quarto (força bruta: a malha do
  Space Setup tem alguns milhares; medir com uma grande). **Get Room Planes / Get Room Plane Info**, **Event On Room Loaded**.
- **Luz e exposição**: a exposição fica **automática** (padrão, Exposição = 0): o histograma do `exposure.comp` já deixa o
  preto de fora (bin 0), então só as coisas virtuais são medidas, como no jogo normal. A primeira versão fixava EV 7 e,
  com a luz assada do nível (lightmaps e volume feitos para o sol de fora), tudo saiu estourado no headset. Um número fixa
  o EV. **Luz de um Quarto** (opcional; padrão: as luzes do nível) põe o sol quente (~3800 K) vindo de cima (pitch −72°) e
  a luz do céu baixa. **Esconder o Chão do Nível**: os planos de chão com `TRANSFORM_NO_RENDER` (colidem).

### Oclusão e sombras no real
- **Profundidade Real** (XR_META_environment_depth): provider + swapchain (D16, 2 camadas), adquirida a cada quadro no tempo
  do display; o renderer copia a imagem do runtime para a sua (`mr_depth`, binding 86, `sampler2DArray`; o layout de
  origem suposto SHADER_READ_ONLY_OPTIMAL) e o composite reprojeta o ponto virtual na câmera de profundidade de cada olho
  (pose e fov dela, não do olho), 4 texels, rampa suave (**Suavidade**, padrão 4 cm), viés 1 cm + 1%. **Sem as Mãos** pelo
  hand removal. Profundidade em GL (n/(1−d) com far infinito; senão a fórmula do NDC [−1,1]); linhas de cima para baixo
  (Vulkan), `vr.MRDepthFlipY` se o runtime vier ao contrário — **a conferir no headset**.
- Sem profundidade real (o Link não dá, ou Oclusão = Planos e Móveis): os **planos e volumes do quarto** escondem o que
  está atrás (raio contra as caixas no composite).
- **Sombras no quarto**: onde o real aparece, o ponto real (da profundidade real; senão da caixa do quarto) recebe o
  `vsm_shadow` do sol: escurece o passthrough (alfa = virtual + (1 − virtual) × sombra, cor preta) — a sombra do personagem
  virtual no chão, na mesa e na parede reais. (O shadow catcher do AR, `ar_plane`, não serve aqui: o real não está na imagem.)

### Modo simulado (`--vr-sim`, `REALENGINE_XR_SIM_ONLY=1`)
- Quarto de mentira de 5 × 4 × 2,6 m (chão, teto, 4 paredes, mesa, sofá, janela, porta; malha com as 8 caixas), feito no
  primeiro tick com a chave ligada (as `--cvar` chegam depois do begin play). O composite desenha o quarto (cor por rótulo,
  luz do teto, grão) no lugar do real, **com o estilo** (P&B, uma cor, ajustado, opacidade): tudo visível no espelho da janela.
- Profundidade real simulada: o quarto visto de cada olho, 160 × 160 por olho, D16 como a da Meta, refeita só quando a
  cabeça mexe: o mesmo caminho da profundidade do headset (cópia, reprojeção, oclusão, superfície das sombras).

- `REALENGINE_MR_NO_ROOM_BOXES=1`: o simulado sem as caixas do quarto (o caminho do headset: a medida); sem quarto nenhum o
  real de mentira é uma parede clara lisa (as bordas do virtual sobre o real vistas na janela).

### Otimização
- As caixas vão ao shader já como a transformação mundo → cubo unitário (três linhas, eixos sobre o comprimento ao quadrado):
  o shader não divide nada; janelas e quarto num laço só. O composite do MR caiu de 0,65 para 0,47 ms por olho no simulado.
- A superfície real para a sombra e o quarto de mentira: a da caixa (exata) antes da profundidade (grossa: degraus).

### Medidas (RTX 3060, `Level.exe --vr-sim`, 2 × 1280 × 1400, MSAA 4×)
| Caso | Pares/s | GPU do par | composite por olho |
|---|---|---|---|
| VR sem MR | 550 | 1,77 ms | 0,127 ms |
| MR, o caminho do headset (profundidade real simulada, oclusão, sombras; sem caixas) | 495 | 1,99 ms | 0,289 ms |
| MR simulado completo (quarto de mentira por caixas, estilo, profundidade, sombras) | 419 | 2,36 ms | 0,470 ms |
- CPU do MR: tick 0,001 ms, envio ao renderer 0,002-0,003 ms (a profundidade simulada só refeita quando a cabeça mexe).
- Autoteste `mr_selftest`: 0 falhas em 4,4 ms. `mr_sim` com validação: limpo.
- **Quest 3S pelo Link** (2026-11-18): o runtime da Meta oferece XR_FB_passthrough (com cor) e o Scene (container, malha;
  sem `scene_capture`); **sem** XR_META_environment_depth e sem ALPHA_BLEND pelo Link. O passthrough liga (`the passthrough
  on`), 72 quadros/s, GPU do par 4,7-4,8 ms. A consulta do quarto passou a funcionar com o filtro de armazenamento LOCAL
  encadeado (como as amostras da Meta; sem ele `xrQuerySpacesFB` recusava) e voltou **sem quarto**: falta o Space Setup no
  headset e/ou "Spatial Data over Meta Quest Link". Um quadro de 239 ms no começo (a partida do passthrough? a medir).
- **Teste do usuário no headset pelo Link** (2026-11-18): o passthrough funciona com os objetos virtuais no quarto.
  1) "Tudo muito brilhoso, estourado": a exposição fixa em EV 7 (corrigido: automática). 2) **Bordas pretas em volta dos
  objetos** sobre o real: no simulado (a mesma conta, parede clara no lugar do real) as bordas saem limpas; trocar para alfa
  não pré-multiplicado (`vr.MRUnpremultiplied`, a flag UNPREMULTIPLIED na camada e a cor dividida pelo alfa) não mudou no
  headset. **Fica para depois** (pedido do usuário). Suspeitas a testar: o formato da swapchain (44 = B8G8R8A8_UNORM, com
  valores já em sRGB: o compositor mistura como linear?), a reamostragem do compositor nas bordas do alfa duro (um pixel:
  sem cobertura parcial no alfa), a cor das amostras do MSAA ponderada pelo brilho (`aa_encode`) puxando a borda para o
  preto do céu.

## Falta (precisa de hardware ou de SDK)
- (Q25b) **Realidade mista no headset**: pelo Link o passthrough liga, mas ninguém olhou ainda com o headset (o alfa sobre o
  passthrough, os estilos, as sombras); o quarto real nunca veio (Space Setup / Spatial Data over Link); a profundidade real
  só existe no Quest sozinho (o APK `-Quest -MixedReality` nunca instalado): conferir a convenção das linhas e o layout da
  imagem de profundidade da Meta (`vr.MRDepthFlipY`, o layout de origem da cópia).
- (Q25 / espelho) A janela do VR simulado (`--vr-sim`) sai de cabeça para baixo na captura (`--shot`), com e sem MR: o
  espelho dos olhos ou a captura invertem o Y (no headset a imagem está certa). Não mexido aqui.
- (Q25) **Quest standalone no aparelho**: o APK do `-Quest` nunca foi instalado: o adb viu o headset uma vez como "offline"
  (o driver ADB da Meta, `Reality Labs Composite ADB Interface`) e sumiu; medir nele (timestamps, 90/120 Hz, Adreno).
- (Q25) Multiview, Application SpaceWarp (vetores de movimento), FOV assimétrico no próprio renderer (sem a folga de 25%).
- (Q25) Controles no headset: teleporte, giros, laser e o painel de menu só testados no simulado e no autoteste; mãos
  rastreadas, mãos físicas, gestos, empurrar e arrasto testados e aprovados pelo usuário no Link (2026-11-18).
- **Testado no S23 Ultra** (2026-11-08, depuração sem fio): a sessão inicia, pede a câmera, rastreia; a imagem atrás do
  nível na orientação certa, os objetos virtuais no quarto com sombra. Ajustes que o aparelho pediu: a ARCore exige a
  `InstallActivity` declarada no manifesto (declarada, nunca aberta); a imagem de CPU padrão (640 × 480) ficava mole: a
  configuração de câmera com a maior imagem até 1920 × 1080; o chão do nível escondido também no celular quando a ARCore
  roda (o chão real recebe as sombras). Antes disso o jogo nem abria no Adreno: notas-dev/android.md. A conferir: os
  planos e as sombras no chão real com um nível pensado para AR, o toque (AR Line Trace).
- Conferir a oclusão no S23 (a profundidade real, as bordas, o custo); a imagem da câmera pelo hardware buffer no Vulkan
  (sem cópia de CPU) e a tela de instalação da ARCore.
