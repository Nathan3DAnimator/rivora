# Partículas (2026-11-07, pedido do usuário)

Pedido: "dois sistemas de partículas, um semelhante ao Niagara e outro semelhante ao do Tlender, hiper otimizados para
mais de 5 milhões de partículas com objetos, malhas e tal, em tempo real, sem queda de FPS; pode se basear no Tlender".

## Os dois sistemas, uma simulação
- **Sistema de Partículas** (`.rparticle`, Niagara): emissores, cada um uma pilha de módulos nos estágios do Niagara
  (Emitter, Particle Spawn, Particle Update, Render). 22 módulos (`particles.c` `g_modules`): Spawn Rate, Spawn Burst,
  Lifetime, Shape Location (ponto, esfera, caixa, cone, disco, malha do ator), Initial Velocity/Size/Color/Rotation,
  Gravity, Drag, Curl Noise, Brownian, Vortex, Point Attraction, Wind, Collision (chão), Speed Limit, Kill Box,
  Color/Size Over Life, Sprite Renderer, Mesh Renderer. 11 modelos (Fogo, Fumaça, Faíscas, Chuva, Neve, Chafariz,
  Magia, Explosão, Poeira, Destroços, Galáxia 5 M). Um ator o toca (`CParticleSystem`, o Niagara Component).
- **Partículas (Tlender)** (`CParticleTl`, o Particle Properties do Blender) num ator com malha: Número entre Quadro
  Inicial e Fim, Vida (+ aleatoriedade), Emitir De vértices/faces/volume da malha do ator, velocidade Normal /
  Aleatória / do Objeto (m/s), física Newtoniana (gravidade, arrasto, browniano), Renderizar Como Halo (brilho ou
  iluminado) ou Objeto (uma malha), Repetir, Espaço Local. Unidades do Blender (metros, quadros).
- Os dois viram o mesmo bloco `GpuEmitter` (`render_gpu.h` = `shaders/particles.glsl Emitter`).

## A simulação (como a do Tlender: nascimentos determinísticos)
- Pool fixo por emissor: a vaga i nasce num tempo que só depende de i (`p_birth`): taxa contínua (i/R, depois a cada
  vagas/R), rajadas (todas ao mesmo tempo, de novo a cada intervalo), Tlender (início + (fim - início) i / N, de novo
  por ciclo com Repetir). Sem lista livre, sem atômico para nascer, o mesmo resultado em toda execução.
- Partícula de 32 bytes: posição, nascimento, velocidade, índice do nascimento. Vida, tamanho, cor e giro saem de um
  hash (semente, vaga, índice).
- `particle_sim.comp`: uma passada por emissor faz nascimento, forças, colisão **e o cull** no mesmo thread (uma leitura
  e uma escrita da partícula): no campo de visão e com mais de 1/8 de pixel, entra na lista de quads, de pontos ou de
  malhas; um ballot de subgrupo por lista (um atômico por subgrupo). As contagens vão para os argumentos indiretos.
- Vórtice como o Vortex Velocity do Niagara: a posição gira em torno do eixo (órbitas estáveis; como aceleração as
  partículas eram arremessadas para fora: 100 mil de 5 M ficavam na tela).
- Desenho no passe principal: malhas antes do céu (com profundidade, iluminadas, sombra do sol pela VSM), sprites
  depois do céu (premultiplicado: soma ou mistura). Sprite pequeno (até 48 px) vira **ponto** (`gl_PointSize`, 1
  vértice; `largePoints` ligado em `vkctx.c`); grande ou alinhado à velocidade, quad de 6 vértices sem instâncias.
  Malha pequena na tela (abaixo de "Sprite Under", 3 px) vira sprite da sua cor.
- **Bug achado:** o bloco de cada emissor ficava em `i * sizeof(GpuEmitter)`, e o `EmitterBuf` declara alinhamento
  16: com dois emissores (Fogo) a GPU caía (device lost). `GpuEmitter` agora tem `_Alignas(16)` e um `_Static_assert`.

## Editor
- Content Browser > Adicionar > Efeitos Visuais (um modelo). Duplo clique abre o editor (`particle_editor.c`):
  emissores, pilha por estágio (o "+" com os 2 recentes e 2 recomendados do `graph_menu`, `GM_PARTICLES`),
  parâmetros do módulo; **Prévia no Nível** toca o sistema na frente da câmera e mostra cada mudança.
- Adicionar Rápido > Efeitos Visuais > Sistema de Partículas (o modelo em `Content/Particles`); arrastar o asset
  para o nível; Detalhes do Particle System e das Partículas (Tlender) (Detalhes > Adicionar num ator com malha).
- No editor os sistemas tocam sempre (os finitos recomeçam); em jogo, como ativados.

## Blueprints (6 nós, Effects|Particles)
Spawn System at Location (o sistema nos Detalhes; nos scripts o primeiro argumento entre aspas), Activate System,
Deactivate System, Set Particle Color, Set Spawn Rate Scale, Is System Active.

## Medidas (RTX 3060, 1440p)
| Cena | Partículas | Resultado |
|---|---|---|
| `--scene particles5m` (galáxia, 5,1 M vagas) | 4,75 M desenhadas | **212 fps**; simulação + cull 1,41 ms, passe principal 4,39 ms (preenchimento), GPU mínima 2,14 ms |
| antes dos pontos e da passada única | 4,75 M | 113 fps (8,3 ms na GPU: 28 M vértices dos quads, 2 ms de sim + cull) |
| `--scene particles` (8 sistemas + Tlender, validação) | ~30 mil | 281 fps, validação limpa |
- CPU: 0,007–0,018 ms por quadro (os emissores; nada por partícula).
- Testes: `particles_selftest`, `particles_scene` (validação, 10 emissores), `particles_5m` (>= 4 M desenhadas, 180 fps).

## Limites / próximos passos
- Sem ordenação dos sprites misturados (soma: não precisa); sem partículas suaves (profundidade); malhas de partícula
  não projetam sombra; colisão só com o chão (a do Niagara pela profundidade da tela fica para depois). (As fitas, Ribbon
  Renderer, já existem; o resto contra o Niagara: auditoria-artista.md, Q23.)
- Os nomes dos parâmetros e as dicas dos módulos ainda em inglês na interface em português.

## Editor como o do Niagara e módulos novos (2026-11-08, pedido do usuário)
Relato: "bugs na UI do editor de partículas, não mostrando o nome, apenas a checkbox; e ainda bem diferente do
Niagara". O bug: o `igSelectable` do módulo tinha largura negativa (`-fs*1.6`), que no ImGui vira um item de largura
negativa sem texto. `particle_editor.c` reescrito:
- Barra: Salvar, Localizar (Content Browser), Tocar/Pausar, Recomeçar, velocidade (0,1x a 2x), Prévia no Nível.
- **Prévia** (esquerda): o sistema tocando sozinho, órbita com o mouse. Amostra de até 500 partículas por emissor na CPU
  com as mesmas contas do `particles.glsl` (nascimento, aleatórios, formas, velocidade, gravidade, arrasto, vento,
  vórtice, atração, colisão, limites, cor/tamanho ao longo da vida, escala pela velocidade); curl e browniano não
  (a imagem exata é a Prévia no Nível). Fogo + faíscas: ~0,3 ms de CPU com a janela aberta.
- **Visão Geral do Sistema**: nó do Sistema (lista de emissores, + Emissor) e um nó por emissor com os grupos coloridos
  do Niagara; clique direito no título: Duplicar / Remover Emissor.
- **Seleção**: Propriedades do Emissor e a pilha inteira com cada módulo desdobrável e o parâmetro como deve ser
  (X/Y/Z → vetor, R/G/B[/A] → cor HDR, "(0 a, 1 b)" → lista, 0/1 → caixa).
- **Curvas** (gradiente da Cor ao Longo da Vida com as paradas arrastáveis; curva do Tamanho com 4 pontos arrastáveis)
  e **Linha do Tempo** (taxa contínua, vida das últimas, rajadas; arrastar percorre a prévia).
- Módulos novos: **Emitter State** (infinito / uma vez / N voltas, duração, atraso; rajada sem intervalo repete a cada
  volta; o atraso vai em `GpuEmitter.extra[3]` e só a CPU usa), **Sub UV Animation** (flipbook na textura do sprite:
  sobre a vida, fps ou aleatório, com mistura), **Scale by Speed** (tamanho e alfa), **Camera Offset**, formas
  **cilindro** e **toro** (`PSHAPE_CYLINDER`/`PSHAPE_TORUS`). `GpuEmitter` ganhou `sub_uv`, `speed_scale`, `extra`
  (+48 bytes por emissor).
- `--open-asset /Content/...rparticle` abre o editor; `REALENGINE_PE_SELECT=e,m` seleciona um módulo (fotos).

## Ribbon Renderer (B10, 09/11)
- Módulo **Ribbon Renderer** (`PM_RIBBON`, estágio Render: Width Scale, UV Repeat, Tail Fade, Lit, Additive, Glow, Opacity,
  material) -> `PF_RIBBON` e `GpuEmitter.ribbon` (+16 bytes, C e GLSL). Modelo **Ribbon Trail** (60/s, 2 s, subindo com
  curl suave).
- **Sem ordenar**: os nascimentos já vêm em ordem nos slots de taxa (o slot s nasce em s / taxa, depois a volta
  seguinte). `ribbon.vert` desenha 6 vértices por slot (`rate_slots` segmentos, sem indireto), o segmento entre o slot s
  e o s+1 (o último volta ao 0); vale se os dois estão vivos (`k` != ~0) e nasceram um depois do outro (0 < diferença <
  1,5 / taxa). A fita olha para a câmera; a largura de cada ponta é o tamanho da partícula; uv.x atravessa, uv.y conta
  os nascimentos (a textura se repete a cada UV Repeat).
- `ribbon.frag`: a textura ao longo (o material), depois o sombreamento dos sprites com o disco suave só no eixo de
  través (`shade_sprite(vec2(uv.x, 0))`); o material leva a forma 2 (quadrado) para o sprite não amostrar de novo.
- **Achado**: com o curl, alguns pares seguidos ficavam a metros um do outro (partículas lançadas longe): linhas longas
  pelo céu. Corte: o segmento só existe se o comprimento cabe em 4 x (|va| + |vb|) / taxa + 8 x largura (um rastro de bala
  rápido continua inteiro).
- Desenho: os sprites indiretos pulam os emissores de fita (`particles_draw_sprites` ganhou o pipeline `part_ribbon`).
- **Medidas**: a cena `particles:8` (a fita sozinha) 508-519 fps, GPU 1,89-1,92 ms; a cena `particles` (11 emissores)
  passa com a fita. Teste: `particles_selftest` confere o modelo como fita.
- **Falta**: partículas suaves (soft particles: a profundidade da cena no passe principal), ordenação dos translúcidos,
  colisão pela profundidade da tela e pelo SDF, sombra das malhas de partícula, tradução dos módulos, o campo de vento
  único (tecido, chuva, partículas), respingos na água rasa.

## Fogo (2026-11-09, plano de melhorias A14)
Ator **Fire** (`COMP_FIRE`, `CFire`, `src/fire.c`, `shaders/fire.glsl`, `fire_sim.comp`, `fire_render.comp`), como o
Grid 3D Gas do Niagara Fluids:
- **Grade própria na GPU** por fogo (células cúbicas, Width × Width × Height cm; o ator a 1/5 da altura acima da base,
  dentro da chama, com a sua **Light** pontual): velocidade, escalares (temperatura sobre o ar, fumaça, combustível, a luz
  do sol que chega), curl, pressão e divergência, 6 vec4 por célula num buffer só (endereço no `pc.vertices`).
- **Um passo por quadro** (dt até 33 ms): advecção semi-Lagrangiana trilinear com combustão (combustível quente queima em
  calor e fumaça), resfriamento, dissipação, a base injetando combustível com ruído; curl; forças (empuxo pelo calor, peso
  da fumaça, confinamento de vórtices); divergência; Jacobi (lados e topo abertos, chão fechado; 24 iterações); projeção;
  a luz do sol pela fumaça até cada célula (a sombra na fumaça).
- **Desenho** depois da subsuperfície e antes dos translúcidos: o raio de cada pixel pela caixa até a profundidade da cena,
  64 passos no máximo, início girado por pixel e quadro (o TSR tira a média); a chama como corpo negro da fuligem
  (temperatura limitada a 900-1900 K: acima disso o brilho de Planck estourava num clarão branco), a fumaça absorvendo e
  espalhando o sol (com a sombra) e o céu. A luz do ator tremula por ruído (Flicker) com cor de 1800 K.
- **Presets:** Fogueira (padrão), Tocha, Fogueira Grande, Vela. Menu Adicionar > Efeitos Visuais > Fire; Details com tudo.
- **Medidas** (3060, Épico, cena `fire`, 40 × 40 × 66 células, 9,8 MB): 231-242 fps × 267 sem (`REALENGINE_NO_FIRE=1`),
  ~0,6-0,8 ms na GPU. Teste `fire`.
- **Falta (A14):** flipbook com vetores de movimento (gravar a simulação), propagação por células de superfície e queima
  dos materiais (como em Zelda BotW), vento do clima, reagir aos personagens (obstáculos), neblina/ar sobre o fogo, VDB
  (A9.5a), custo por timestamps no Debug Viewer.

## Volumes VDB: o Heterogeneous Volume (2026-11-09, plano de melhorias A9.5a)
Ator **Heterogeneous Volume** (`COMP_VOLUME`, `CVolume`, `src/volume.c`), como o da Unreal: um `.vdb` ou sequência
(os dígitos do nome, como o `fire_0001.vdb` do Blender) lido pelo **leitor de OpenVDB do Tlender** (`src/vdb.c`,
`src/volume_grid.c`, `src/vg_math.h`: copiados e adaptados, com o zlib e os arquivos da engine; zip e blosc/LZ4).
- **Carga:** todos os quadros (até 240) reamostrados para uma grade densa (Resolution células no lado maior, células
  cúbicas, a caixa que os arquivos cobrem + uma célula de ar) em half floats: temperatura (`temperature` em K, ou ×
  escala; `flame`/`flames` × Flame Temperature) e densidade (`density`). Eixos Z ou Y, unidade em cm.
- **Desenho:** pelo `fire.c` como um fogo sem simulação: o quadro atual num staging por quadro em voo, copiado para os
  escalares quando muda, o passe 6 (o sol pela fumaça) e o mesmo `fire_render.comp`.
- **Exemplo:** `Volumes/Example_Puff_0001..0024.vdb` (um puff subindo de um núcleo quente) gravado pelo escritor do
  Tlender e lido de volta (a ida e volta testada). Cena `volume`, teste `volume_vdb`.
- **Medida** (3060, Épico): 56x56x66 células, 207 fps na cena; carga de 24 quadros 1,4 s.
- **Falta:** carga em segundo plano/streaming de sequências longas, GI e neblina volumétrica iluminando o volume, NanoVDB,
  grades de velocidade (motion blur).
