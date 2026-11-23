# Android — exportar o jogo (APK) e controles de toque

O jogo empacotado roda no Android pelo mesmo runtime em C + Vulkan (`realengine_game`, aqui uma `.so` arm64 carregada
pela `NativeActivity`). O editor continua só no Windows. Assistente: `android-export`, `touch-controls`.

## Como sai o APK
1. **Compilar > Empacotar para Android** (ou `realengine --open L.rlevel --package-android <pasta>`): faz o pacote do
   Windows (`editor_package_android` → `package_prepare`/`package_run`, `src/editor.c`) e, no mesmo worker, roda
   `android/package_apk.ps1` por `re_run_command` (CreateProcess sem janela, saída em `<pasta>/<jogo>_android.log`).
2. `package_apk.ps1` (sem Gradle): `aapt2 link` com o manifesto preenchido (`@TITLE@`, `@ORIENTATION@`,
   `--rename-manifest-package`), ícone em mipmaps, `assets/game/**` (o pacote sem .exe/.dll/.pdb) + `assets/manifest.txt`
   + `assets/build.txt`, `lib/arm64-v8a/librealengine_game.so`, `classes.dex`, `zipalign`, `apksigner`.
   Sem keystore no projeto: chave de depuração criada uma vez em `%APPDATA%\RealEngine\android\debug.keystore`.
3. `android/prebuilt/` (fora do git, levado pelo `make_dist.ps1`): a `.so` sem símbolos e o `classes.dex`. Faltando, o
   script compila (`-BuildNative`: NDK 27, API 30, Ninja do VS, `build-android/`) e chama `javac` + `d8`.
4. No aparelho, `platform_android.c` extrai os assets de `manifest.txt` para `<files>/game` uma vez por build
   (`.build` com o carimbo de `build.txt`); o jogo lê dali como no PC (`re_exe_dir` = `<files>/game`).

Configurações do Projeto > Plataformas > **Android**: nome do pacote (vazio: `com.realengine.<nome>`), orientação
(sensorLandscape / sensorPortrait / fullUser), Version Code, keystore + alias + arquivo da senha, Controles de Toque.

## A camada Android (`android/`)
- `glfw_android.c`: a GLFW do Tlender Mobile adaptada (janela = a tela, teclado, toques → cursor, monitores falsos, e
  `platform_touch_points`: todos os dedos do evento, `re_vibrate_ms`).
- `vk_android.c`: a `libvulkan` do Android só exporta 1.1; as funções 1.2/1.3 (dynamic rendering, synchronization2,
  BDA, timeline, draw indirect count) vêm de `vkGetDeviceProcAddr` depois do `vkCreateDevice`. `re_compat.h` (incluído
  à força fora do MSVC) renomeia as chamadas para `re_*`. Sem elas o jogo sai com mensagem (Vulkan 1.3 obrigatório).
- `platform_win32.c`/`platform_files.c` ganharam o ramo POSIX (arquivos, threads, atômicos, `re_sync.h`), `audio.c` o
  AAudio (callback chama o mesmo `mix()`), `mat_image.c` o `AImageDecoder`, `net_udp.c` sockets BSD, `bp_jit.c` só no
  x64 (no ARM o bytecode roda no interpretador), `steam.c` desligado.
- `GameActivity.java`: só o que precisa de Java (teclado na tela, tela cheia, pastas, idioma, HTTP, links, área de
  transferência, vibrar).

## Controles de toque (`src/ui_widget_draw.c`)
Três widgets novos na Paleta: **Virtual Joystick** (texto `Move` ou `Look`), **Touch Button** (texto = tecla, dica =
rótulo; `bp_key_event` na borda de apertar/soltar, eventos OnPressed/OnReleased) e **Touch Look Area** (arrasto gira a
câmera; `value` = graus por pixel). Cada dedo fica preso ao widget onde tocou (`g_hold`) até sair; os eixos do quadro
(`ui_touch_axes`) somam aos do teclado/mouse em `bp_set_input`. No PC o mouse é um dedo (`REALENGINE_TOUCH=1` liga os
controles no PIE). `ui_touch_controls_asset()` cria `/Content/UI/WBP_TouchControls.rbp` (joystick, área de olhar,
Pular/Usar/Correr/Pausa = Space/E/Shift/Escape); `project_begin_play` o põe na tela no Android quando ligado.
No Android o mouse-look e os botões do mouse ficam desligados (`game_main.c`).

## Texturas
GPUs de celular (Mali, Adreno) em geral não leem BC: `VkCtx.bc_textures` vem de `textureCompressionBC`; sem ele o
pipeline `bc7` nem é criado e os materiais ficam em RGBA8 (o caminho de `REALENGINE_NO_BC7`), 4× a memória.

## O compilador de shaders do Adreno (S23 Ultra, Adreno 740, 2026-11-08)
O jogo fechava depois da tela de carregamento: o driver (`libllvm-qgl.so`) travava (SIGSEGV) ou recusava (`-13`,
VK_ERROR_UNKNOWN) shaders que o PC aceita. Achado no aparelho pela **depuração sem fio** (`adb pair`/`connect`, o
logcat do jogo) e por uma sonda que roda no celular (`adb shell`) e só cria pipelines a partir de `.spv` (compute e
vertex+fragment com o layout do motor): dezenas de variantes testadas em segundos, sem APK.
- **Componente de vetor por índice variável** (`v[i]` num `vec4`/`uvec4`/`ivec4`) mata o compilador: `f.water[10][c]`,
  `md[ax]`, `cluster_count[lod]`, `vertex_offset[l]`, `e.size_life[s]`, `d[ax]` da grade. Agora escolhas explícitas
  (`W_CASCADE_SIDE`, `u4at`/`i4at` no `cull.comp`, seletores). **Arrays** (`vec3 c[6]`, `lights[i]`) passam.
- **`barrier()` numa função chamada de um `switch` com outros passes** (a FFT do oceano): só compila sozinha. A FFT
  virou um pipeline próprio (`water_sim.comp` com `-DRE_WATER_FFT`, `water_fft.comp.inc`).
- **Memória compartilhada**: 32 KB no Adreno (48 no PC). O `cull.comp` passava 12 bytes (`s_cnt[8192]` + 3 uints);
  agora `s_cnt[8184]` (2 × 1023 malhas × 4 LODs).
- **`v.normal_oct` lido pela struct `Vertex`** no `particle_mesh.vert`: o pipeline era recusado (o `mesh.vert` lê o
  mesmo campo sem problema); lido como o 4º `uint` do vértice, passa.
- Quando um pipeline é recusado o log agora diz qual (`pipeline refused: vertex s_..., fragment s_...`): `SHADER()`
  guarda o nome do array de cada módulo.
- Ao escrever shader novo: nada de `vetor[variável]`, e conferir no celular (a sonda está no scratchpad da sessão;
  refazê-la é um arquivo C de ~150 linhas com `vkCreate*Pipelines`).

## Medidas (2026-11-07)
- Teste `touch_selftest` (`--touch-test`): direções do joystick, dois dedos ao mesmo tempo, tecla segurada e solta,
  arrasto na área de olhar; **0,14 µs por quadro com 10 dedos**.
- `librealengine_game.so` arm64 sem símbolos: 10,4 MB; APK do modelo vazio: 4,2 MB; APK + .so recompilada pelo
  editor em ~1 min (o build-android incremental).
- Emulador (AVD Android 15, `-gpu host`, Vulkan 1.3 da RTX 3060): abre, desenha o nível com os controles; 1º quadro em
  11,1 s com o cache de pipelines frio (35 pipelines em 5,7 s), áudio AAudio 48 kHz.

## Limites conhecidos
- Não testado num aparelho real (só no emulador). Recursos que o aparelho precisa: Vulkan 1.3, bufferDeviceAddress,
  descriptor indexing, scalarBlockLayout, multiDrawIndirect; ray tracing e mesh shaders são opcionais (já têm fallback).
- Sem JIT no ARM; Steam não existe no Android; diálogos de arquivo não existem no jogo.
- APK, não AAB (a Play Store pede AAB para apps novos: dá para converter com o bundletool).

## Rodar liso (2026-11-07, pedido: "sistemas para o Android rodar liso, otimização focada no Android")
Código: `src/mobile.h/.c` (no `re_render`; ligado no `game_main.c`), `android/glfw_android.c` (escala, taxa, memória baixa),
`src/vkctx.c` (superfície perdida e recuperada), `src/audio.c` (`audio_device_suspend`), `material_set_texture_budget`.
- **Perfil por aparelho** (`mobile_tier_for`): Adreno ≥ 730, Mali-G ≥ 710, Immortalis, Xclipse = Alta; Adreno ≥ 640,
  Mali-G610/G7x = Média; o resto Baixa; < 4 GB de memória força Baixa, < 6 GB no máximo Média. Ou a escolha do projeto
  (Project Settings > Android > Qualidade no Celular). `mobile_apply`: sem efeitos por raio nem surface cache, nuvens em
  panorama, neblina só na Alta, sombras virtuais 4/5/6 níveis de 1024/1024/2048 e 4/8/12 amostras, LOD 6/4/2 px.
- **Escala da tela** (`r.MobileContentScaleFactor`): `ANativeWindow_setBuffersGeometry` em 70/80/90% (o compositor amplia
  de graça); `glfwGetFramebufferSize` dá o tamanho dos buffers e os toques vêm multiplicados (`GX`/`GY`). Refeita em cada
  janela nova (`APP_CMD_INIT_WINDOW`, `WINDOW_RESIZED`).
- **Resolução dinâmica** (`mobile_res_step`): janelas de 0,5 s com a média da GPU; acima de 92% do orçamento desce na hora
  (pela raiz do excesso, passos de 5%), abaixo de 70% por 4 janelas sobe 5%. Cada passo realoca os alvos do quadro
  (uma pausa curta): poucos passos, só sustentados. Faixas TSR 50-70 / 55-80 / 60-90%.
- **Ritmo**: FIFO no Android, `ANativeWindow_setFrameRate` (pelo `dlsym`: o stub do NDK 27 não tem) e um prazo contínuo
  por quadro (sem deriva; atrasou, recomeça). No PC com `REALENGINE_MOBILE=1`: 60,0 fps exatos.
- **Térmica** (`AThermal_*` pelo `dlsym`: o `thermal.h` do NDK 27 não compila em C): uma vez por segundo; folga ≥ 0,85 ou
  status MODERATE tira 10% do teto da resolução, ≥ 0,95 ou SEVERE corta a taxa pela metade (mín. 30); volta abaixo de 0,75.
  **Economia de bateria** (`PowerManager.isPowerSaveMode` pelo Java, a cada 5 s): 30 fps. **ADPF**
  (`APerformanceHint`, Android 13+): o tempo de CPU de cada quadro informado.
- **Segundo plano**: a superfície e o swapchain destruídos no `APP_CMD_TERM_WINDOW` e refeitos com a janela nova; o
  AAudio fechado e reaberto. **Memória baixa** (`APP_CMD_LOW_MEMORY`): o orçamento de texturas cede 1/4 (mín. 128 MB);
  orçamento inicial 384/640/1024 MB, no máximo 1/4 da memória.
- Medir: logcat a cada 5 s (`Mobile: fps, CPU, GPU, resolução de WxH, térmica`); `adb shell setprop debug.realengine.tier
  1|2|3`, `debug.realengine.fps N`, `debug.realengine.mobile 0`; no PC `REALENGINE_MOBILE=1`, `REALENGINE_MOBILE_TIER`,
  `REALENGINE_MOBILE_FPS`, `REALENGINE_MOBILE_HEADROOM` (calor de mentira).

### Medidas (2026-11-07)
- Teste `mobile_selftest` (`--mobile-test`): 9 GPUs/memórias no nível certo; a resolução com carga que segue os pixels
  assenta em 70% (12,7 ms de 16,7) em 1 passo, sem ficar subindo e descendo; o relógio só sobe depois de 2 s calmos.
- PC (`REALENGINE_MOBILE=1`, RTX 3060): 60,0 fps (16,62 ms) com o ritmo; folga térmica 0,97 simulada → 30 fps.
- Emulador (Android 15, 2400x1080): swapchain 2160x972 (90%); ~20 fps nos dois modos (19,8 sem o perfil, 22,9 com):
  o teto é a apresentação do emulador, a GPU fica em ~1,7 ms. Home e volta: janela destruída, áudio solto, tudo
  refeito no mesmo processo. Abertura 5,6 s (3,6 s são a intro animada, desligável).

## Google Play (2026-11-07, pedido: "exportação de games para a play store")
- `package_apk.ps1` agora faz **APK e/ou AAB do mesmo módulo**: `aapt2 link --proto-format` (o do bundle), e o APK é ele
  convertido (`aapt2 convert`), então instalar o APK testa o que vai para a loja. O AAB: `BundleConfig.pb` (escrito à
  mão: bundletool 1.17.2), `base/manifest`, `resources.pb`, `res`, `dex`, `lib`, `assets`. Acima de 150 MB de arquivos do
  jogo: **asset pack install-time** `game_assets/` (manifesto `dist:module` em proto); o `manifest.txt` fica na base.
  Assinado com `jarsigner` (SHA256withRSA) e a chave de upload. Com o bundletool em
  `%APPDATA%\RealEngine\android\bundletool.jar` (ou `BUNDLETOOL`), roda o `validate`.
- **SDK alvo 36** (exigido pela Play desde 31/08/2026; compila com a android-35), **16 KB**: `-z max-page-size=16384`
  (segmentos LOAD alinhados a 0x4000) e `zipalign -P 16`. Biblioteca comprimida e extraída na instalação
  (`extractNativeLibs=true`: o .NET do PowerShell não grava entrada *stored* de verdade).
- **Ícone adaptativo** (`mipmap-anydpi-v26`: a figura em 60% de 108 dp sobre `ic_launcher_background`). **Imagens da loja**
  (`-StoreAssets`): ícone de 512 px e gráfico de destaque 1024x500 (da imagem de carregamento do projeto, senão logo +
  título).
- **Chave de upload**: Project Settings > Android > Create Upload Key (`-NewUploadKey`: PKCS12, RSA 4096, 10000 dias,
  senha aleatória no `.pass`) em `<projeto>/Build/Android/`; o projeto aponta para ela. Compilar > **Empacotar para
  Google Play (AAB)** exige a chave, gera `<jogo>.aab`, `<jogo>.apk` e `<jogo>_GooglePlay/`, e sobe o Version Code.
  `--package-play <pasta>` e `--android-upload-key` na linha de comando.
- **Checklist** na página: pacote próprio (não `com.realengine.*`/`com.example`), chave, ícone, política de privacidade;
  SDK 36, 64 bits, 16 KB e AAB marcados como feitos pela engine.
- Testado: chave criada, AAB assinado (`jarsigner -verify`: CN do projeto), Version Code 1 → 2, APK do mesmo módulo
  instalado e rodando no emulador; o asset pack forçado (`-AssetPack on`) monta `game_assets/` com os 26 arquivos.
  Validado no bundletool 1.18.3 (2026-11-09: `--package-play` de um projeto em branco, "AAB conferido pelo bundletool"). Falta:
  enviar um AAB de verdade ao Play Console (precisa da conta do desenvolvedor).
