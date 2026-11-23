# Áudio

Sistema de áudio próprio, com as peças e os nomes da Unreal. Feito em 2026-11-06 em quatro fases: núcleo, som 3D,
mixagem e integração.

## Arquivos
| Arquivo | O quê |
|---|---|
| `src/audio.h`, `audio.c` | Dispositivo WASAPI, mixer, vozes, espacialização, sons ativos, concorrência, virtualização, mixes, streaming |
| `src/audio_dsp.h/.c` | Efeitos: biquads, reverb FDN, EQ, compressor, delay, filtro, limitador do master |
| `src/audio_wave.c` | Sound Wave (`.rsound`): grava, mapeia, acha pelo hash |
| `src/audio_assets.c`, `audio_internal.h` | Attenuation, Class, Mix, Concurrency, Submix e Sound Cue (JSON); classes da engine; avaliação do Cue |
| `src/audio_world.h/.c` | Audio Component, Audio Volume, ouvinte, oclusão, On Audio Finished, chamadas dos Blueprints |
| `src/audio_import.c` | Importador: WAV próprio, Media Foundation para o resto |
| `src/audio_editor.h/.c` | Editor dos assets, grafo do Cue, miniaturas, menu Adicionar > Sounds, Details dos componentes |
| `tests/test_audio.c` | Teste do mixer (`audio_mixer`) |
| `src/audio_test.c` | Teste no Play (`audio_play`, `--audio-test`) |

## Núcleo
- **Dispositivo:** WASAPI compartilhado, por evento (período ~10 ms, buffer de 20 ms), float estéreo na taxa do
  dispositivo. O Windows converte para 5.1/7.1 (o par frontal recebe o som).
  - Thread própria com prioridade "Pro Audio" (avrt).
  - Dispositivo perdido ou o padrão trocado (fone plugado; checado a cada 2 s): reabre sozinho.
  - Com `REALENGINE_BACKGROUND`, `REALENGINE_NO_AUDIO` ou sem dispositivo: dispositivo silencioso. É a mesma mixagem,
    no ritmo do relógio, sem sair som (os testes não fazem barulho no PC).
- **Sound Wave (`.rsound`):** cabeçalho de 256 bytes (formato, taxa, loop, volume, tom, prioridade, classe, atenuação,
  concorrência, pico, RMS, contorno de 128 pontos para as miniaturas) seguido de PCM 16 bits intercalado.
  - O arquivo é mapeado: nada é carregado, o sistema operacional pagina.
  - Sons longos (> 15 s ao importar, a flag Streaming) têm os próximos 2 s pré-carregados com `PrefetchVirtualMemory`,
    4 vezes por segundo, à frente de onde tocam. Isso é o streaming.
- **Vozes:** 256 vozes do mixer e 256 sons ativos; um Cue pode usar até 4 vozes.
  - O jogo manda iniciar e parar por uma fila sem lock. Os parâmetros são publicados a cada quadro e rampados por
    bloco de 256 amostras (sem cliques).
  - Reamostragem cúbica (Hermite), com caminho rápido longe das bordas.
  - Filtros de um polo: passa-baixa (ar, oclusão, classe) e passa-alta.
- **Virtualização:** as mais altas por prioridade são reais, até os "Max Channels" do projeto (64). As demais e as
  silenciosas (< -80 dB) são virtuais: o tempo corre, nada é mixado, e voltam com rampa a partir do silêncio.
- **Handle:** geração de 15 bits e índice de 9. Fica abaixo de 2^24 e é exato num float de Blueprint.

## Surround 5.1 / 7.1 (2026-11-06)
O mixer tem de 2 a 8 canais, os do dispositivo. O WASAPI é aberto no formato de mistura do próprio dispositivo (sua
máscara de canais).
- **Layout:** cada bit da máscara vira um canal na ordem do WASAPI (FL FR FC LFE BL BR SL SR).
  - 5.1 (surrounds nos bits de trás ou dos lados): ±110°.
  - 7.1: lados a ±90°, traseiros a ±150°. Quad: ±45° e ±135°.
  - Mais de 8 canais, máscara desconhecida ou falha ao abrir: estéreo (o Windows converte).
  - `REALENGINE_AUDIO_STEREO` força o estéreo antigo, para medir.
- **Panorâmica:** VBAP (Pulkki) no par de alto-falantes em volta do ângulo, com potência normalizada.
  - A elevação espalha o som por todos os alto-falantes (cos el).
  - A parte 2D vai ao par da frente; a fonte estéreo 3D vira dois emissores.
  - Ganhos rampados por canal a cada bloco.
- **LFE:** os sons 3D enviam 25% ao subwoofer; o canal passa por um passa-baixa de 120 Hz no master.
- **Reverb:** a entrada soma todos os canais menos o LFE.
  - Saídas descorrelacionadas: a frente usa as linhas 0-3 da FDN, os traseiros as linhas 4-7, os lados a diferença.
  - O centro só recebe primeiras reflexões e o LFE não recebe nada.
  - Em estéreo, a mistura é a mesma de antes.
- **Efeitos e limitador:** em todos os canais. O compressor e o limitador são ligados (o mesmo ganho em todos); o EQ, o
  filtro e o delay são por canal (o ping-pong só no par da frente).
- **Binaural:** só com saída estéreo (fones). Em surround, VBAP.
- **Teste** (`audio_mixer`, dispositivo silencioso com `REALENGINE_AUDIO_CHANNELS=6` e `8`):
  - o canal mais alto é o da direção (frente: FC; ±30°: FL / FR; traseiros; lados no 7.1);
  - o som 2D fica só no par da frente;
  - o LFE recebe um som 3D de 60 Hz;
  - o reverb de um som na frente chega aos traseiros.
  - Custo: 0,036 ms por período de 10 ms com a cauda do reverb, em 5.1 e em 7.1.

## Som 3D
- **Panorâmica:** equal-power pelo azimute no espaço do ouvinte. Direita = up × forward (+Y com yaw 0, como na Unreal).
- **Binaural (Project Settings > Audio):** modelo de cabeça esférica.
  - Atraso interaural de Woodworth (até 0,66 ms), linha de atraso fracionária por ouvido.
  - Sombra da cabeça de Brown–Duda por ouvido: um polo e um zero, bilinear, alfa pelo ângulo até o eixo do ouvido.
  - Atrás: um pouco mais escuro.
  - Uma fonte estéreo vira dois emissores, separados pelo "3D Stereo Spread".
- **Attenuation:** funções Linear, Logarithmic, Inverse, Log Reverse e Natural Sound (dB até -60 no fim).
  - Raio interno e distância de queda.
  - Raio não espacializado: mais perto que ele, o som mistura para 2D.
  - Absorção do ar: passa-baixa interpolado em escala log.
  - Oclusão: raio às caixas de colisão e landscapes, 10 Hz, suavizado; volume e passa-baixa.
  - Envio de reverb pela distância, Doppler (0 a 1) e prioridade atenuada.
  - Sem attenuation num som 3D: a do projeto, senão a da engine (4 m internos, 36 m de queda, Natural Sound).
- **Audio Component (`CAudio`):** ator Ambient Sound ou componente de Blueprint.
  - Toca no Begin Play quando Auto Activate está ligado e segue a entidade.
  - Ao terminar, chama os Custom Events `OnAudioFinished` e `OnAudioFinished_<nome do componente>` no dono.
- **Audio Volume (`CAudioVolume`):** caixa pelo transform (100 cm por unidade de escala).
  - O ouvinte dentro ouve o reverb do volume, misturado no Fade Time; há presets Sala, Salão, Caverna e Ao Ar Livre.
  - Zona ambiente: o volume e o passa-baixa externos (sons de fora ouvidos de dentro) e internos (o contrário).

## Mixagem
- **Sound Class:** hierarquia (volume, tom, passa-baixa, submix padrão, UI, Always Play).
  - Classes da engine: `/Engine/Audio/SC_Master` e, abaixo dele, Music, SFX, Voice, UI e Ambient.
  - Sons sem classe vão para SFX; importados longos vão para Music.
  - Passive Sound Mix: aplicado enquanto um som da classe toca acima do limiar (ducking).
- **Sound Mix:** ajustes por classe (volume, tom, passa-baixa, aplicar aos filhos), fade de entrada e saída, duração.
  Há o mix base e os empilhados, com Push, Pop e Clear.
- **Sound Concurrency:** Max Count e as 7 regras da Unreal (Prevent New, Stop Oldest, Stop Farthest then Prevent /
  Oldest, Stop Lowest Priority (then Prevent), Stop Quietest), mais Limit To Owner, Retrigger Time e Volume Scale.
  - A concorrência padrão do projeto vale por som (como na Unreal): 16 do mesmo som, farthest then oldest.
  - Um asset de concorrência agrupa todos os sons que o usam.
- **Submix:** até 16 barramentos com até 4 efeitos cada; o filho vai ao pai e todos chegam ao master.
  - Efeitos: Reverb, EQ de 4 bandas, Compressor (look-ahead, joelho), Delay (ping-pong) e Filtro (LP, HP, BP).
  - O envio de reverb tem barramento próprio, com o reverb dos Audio Volumes.
  - O master termina num limitador look-ahead de 1,5 ms com teto de -1 dBFS.
- **Reverb:** FDN de 8 linhas (matriz de Householder) com passa-baixa no laço (graves decaem mais devagar).
  - Pré-atraso, 8 primeiras reflexões e 4 allpasses de difusão.
  - O tamanho escala as linhas; o decaimento sai do RT60.
- **Sound Cue:** grafo com Output, Wave Player (loop), Random (pesos, sem repetir), Modulator (faixas de volume e tom),
  Mixer (volume por entrada), Concatenator, Looping e Delay.
  - Avaliado a cada toque em até 4 ondas, com atrasos exatos por amostra.
  - O Concatenator soma as durações; o Mixer toca todas as entradas.

## Integração
- **Content Browser:**
  - Tipos novos (Sound Wave, Cue, Attenuation, Class, Mix, Concurrency, Submix), com cores e ícone de alto-falante.
  - Importa WAV, MP3, OGG, FLAC, M4A, AAC, WMA e Opus. OGG e Opus precisam das Web Media Extensions do Windows.
  - Miniatura com a forma de onda e botão de tocar sob o mouse.
  - Adicionar > Sounds cria os assets.
- **Editor dos assets:** uma janela por asset.
  - Sound Wave: forma de onda, clique para tocar dali, laço, informações e ajustes.
  - Attenuation: todos os campos e a curva de queda.
  - Class, Mix, Concurrency, Submix: a cadeia de efeitos.
  - Cue: grafo com pan e zoom, ligar arrastando, soltar Sound Waves do browser, botão direito para adicionar ou
    desligar, Delete para remover.
  - Espaço toca e Ctrl+S salva.
- **Animações:** notify "Play Sound" (Adicionar Tocar Som).
  - Som próprio, volume, tom, osso (Attach Name) e Follow, independente do Anim Blueprint.
  - Vale nas Montages e no Motion Matching.
  - Tocado na prévia do editor de animação.
  - `anim_bone_entity` acha a parte da malha presa ao osso (ou ao mais próximo acima).
- **Widgets:**
  - Sons por widget: Hovered, Pressed, e Clicked (botões) ou Changed (check box, slider, combo). Tocam como som de UI,
    ouvidos com o jogo pausado.
  - Widget Animations ganham chaves de som ("+ Som" no Designer), tocadas quando a animação passa por elas, nos dois
    sentidos.
- **Blueprints (24 nós):**
  - Play Sound 2D / at Location; Spawn Sound 2D / at Location / Attached (com osso), que devolvem um handle (int).
  - Stop Sound, Set Sound Paused, Is Sound Playing, Set Sound Volume / Pitch.
  - No Audio Component: Play, Stop, Fade In, Fade Out, Set Paused, Set Volume / Pitch Multiplier, Is Playing,
    Set Sound.
  - Mix: Push / Pop / Clear Sound Mix Modifier, Set Base Sound Mix e Set Sound Class Volume (para as opções de volume
    do jogo).
  - O asset vai pelo nome no Details (o texto do nó tem 48 caracteres) e é achado ao executar.
- **Sequencer:** trilha Audio (trilha mestre 2D, ou no ator ligado); a chave guarda o som pelo nome, o volume e o tom.
  Os sons param com a sequência.
- **Play no editor e jogo empacotado:**
  - O ouvinte é a câmera do Play.
  - Pause pausa os sons do jogo (não os de UI nem as prévias); sair do Play para tudo e limpa os mixes.
  - O pacote leva `.rsound`, `.rscue`, `.rsatt`, `.rsclass`, `.rsmix`, `.rsconc` e `.rsubmix`.
- **Project Settings > Audio:** Max Channels, Master Volume, Binaural (fones), o dispositivo e o custo.
- **Debug Viewer > Audio:** ms da thread do mixer por período, vozes reais e virtuais, pico, limitador e cada som
  tocando (distância, dB).

## Medidas (RTX 3060 / CPU de desenvolvimento, 48 kHz)
| Caso | Custo do mixer |
|---|---|
| 200 vozes 3D (64 reais, 136 virtuais), panorâmica | 0,17 ms por período de 10 ms (1,7% de um núcleo) |
| `--audio-test` (cena de teste no Play) | 0,086 ms por período, pior caso 0,19 ms |
| Editor vazio (`--scene house`, Épico) | nenhuma mudança mensurável no quadro (489 fps) |

O mixer roda na sua própria thread: o quadro do jogo só paga `audio_update` (parâmetros, ~µs por som).

## Testes
- `audio_mixer` (`re_test_audio`, dispositivo silencioso guiado pelo teste):
  - nível 2D;
  - panorâmica direita / esquerda;
  - distância;
  - binaural a 4 kHz (a sombra da cabeça só aparece em agudos; a 440 Hz são ~2 dB, como na vida real);
  - limitador (8 senoides altas abaixo de -1 dBFS);
  - cauda do reverb e o seu decaimento;
  - Cue (Random sem repetir, Concatenator a 0,3 s);
  - concorrência (Stop Oldest);
  - 200 vozes com 64 reais;
  - fade in.
- `audio_play` (`--audio-test`):
  - Ambient Sound ouvido na distância certa;
  - On Audio Finished de um componente de Blueprint;
  - Spawn Sound 2D com handle;
  - custo do mixer;
  - nada tocando depois de sair do Play.

## MetaSounds (B8, 09/11)
- `src/metasound.c/.h`: o asset **MetaSound Source** (`.rmsnd`, JSON) é uma lista de nós (o 0 é o Output): Oscillator
  (seno/serra/quadrada/triângulo, Pulse Width), Noise (branco, rosa de Paul Kellet, marrom), Impulses (cliques com taxa e
  altura aleatórias: gotas, estalos), Filter (biquad RBJ passa-baixa/alta/banda, coeficientes uma vez por bloco), ADSR,
  Multiply, Add, Map Range, Smooth (um polo), Distortion (tanh normalizada) e Input (parâmetro com nome e padrão). Ao
  carregar, uma busca em profundidade a partir do Output ordena os nós num programa (ciclo ou ligação inválida: erro no
  editor); o que não chega ao Output não roda.
- **Na thread do mixer**: a voz de um MetaSound tem um `SoundWave` sintético (mono, `ms` apontando o asset, infinito); o
  `mix_voice` chama `ms_render` no lugar do `fetch` (sem reamostrar; o pitch escala os osciladores). Estado por voz
  (`MsState`: fases, memórias dos filtros, envelopes, o último valor de cada input) e um buffer por nó compartilhado (uma
  voz por vez): nada é alocado. Os inputs: `M.msp[voz][i]`, escritos pela thread do jogo (`audio_set_param`) e lidos a
  cada bloco, deslizados ao longo do bloco (sem estalos). Voz virtual: o grafo para.
- **Prontos** (`ms_init`, feitos quando faltam em `Content/Audio/MetaSounds`): MS_Engine (RPM, Load: serra no disparo de
  um 4 cilindros, seno uma oitava abaixo, quadrada uma acima, passa-baixa que abre com a carga, ruído de admissão,
  distorção pela carga), MS_Rain (Intensity: rosa filtrado + gotas por impulsos num passa-banda), MS_Wind (Speed: marrom
  num passa-banda que passeia por um LFO, as rajadas).
- **Sons do mundo** (`audio_world.c`, `auto_sounds`): cada veículo com o motor ligado toca o MS_Engine na posição dele
  com RPM e o acelerador (`VehInfo.throttle`, novo); o ator Rain toca MS_Rain (2D) pela intensidade e MS_Wind pelo vento
  dele. Um Audio Component no ator (ou um componente dele) desliga o automático; `REALENGINE_NO_AUTO_SOUNDS` desliga
  todos.
- **Nó** Set Float Parameter (Audio Component, nome, valor) e `audio_set_param(handle, ...)`; o Content Browser ganhou o
  tipo MetaSound Source (Adicionar > Sons), os seletores de som aceitam MetaSounds, Spawn Sound pelo nome também.
- **Editor**: a janela do áudio mostra os Inputs (arrastar muda a prévia ao vivo) e os nós como linhas (cada entrada:
  valor ou a saída de outro nó; modo; apagar; + Adicionar Nó); uma mudança de ligação recompila e para a prévia.
- **Medidas** (CPU de desenvolvimento, 48 kHz): o MS_Engine (21 nós) 39 µs por 10 ms de som numa voz; 1000 -> 6000 RPM:
  82 -> 572 cruzamentos por segundo; chuva 2 -> 60 mm/h: rms 0,004 -> 0,117; vento 2 -> 20 m/s: 0,009 -> 0,053. A cena
  `vehicles` toca 11 motores (11 ligados). Testes: `audio_mixer` (offline, custo, pelo mixer com `audio_set_param`),
  `vehicles_scene` (os motores ouvidos).
- **Falta**: o grafo desenhado como nós (hoje linhas), Wave Player e Delay dentro do grafo, saída estéreo, gatilhos
  (triggers) e o resto dos nós da Unreal; respingos de objetos na água, ondas do oceano por distância, HRTF para o VR (B8c).

## Pendências e ideias
- HRTF por conjunto de medidas (SOFA); o binaural atual é de modelo esférico (ITD e sombra de cabeça).
- Compressão no `.rsound` (ADPCM ou Opus) para economizar disco; hoje é PCM 16 bits mapeado.
- Passos por superfície (Physical Material) no notify.
- MetaSounds (síntese procedural) não entrou.
