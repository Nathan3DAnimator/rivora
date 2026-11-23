# Sequencer (sugestão #45, 2026-11-04)

O Level Sequence da Unreal: uma cinemática como trilhas de chaves numa linha do tempo. Toca na prévia do editor, no
Play in Editor e no jogo empacotado.

## Asset `.rseq` (`src/sequencer.c`)
- O arquivo é JSON (`format: "RealEngine Level Sequence"`, `version 1`) com `length` (s), `fps` (os quadros em que as
  chaves encaixam no editor) e `tracks`.
- Cada trilha tem `type`, `actor` e `keys`. A trilha se liga a um ator do nível **pelo nome** (o do Outliner, como o
  binding da Unreal). As trilhas mestre (Cortes de Câmera e Evento sem ator) não têm vínculo.
- Tipos de trilha:
  - **Transformação**: posição, rotação e escala, cúbica entre as chaves (tangentes Auto, Catmull-Rom, planas nas pontas).
  - **Visibilidade**: degrau, liga e desliga o Hidden In Game.
  - **Intensidade da Luz** e **Campo de Visão**: lineares.
  - **Animação**: a chave toca uma Animation Sequence no Skeletal Mesh; "Repetir o Clipe" faz o clipe repetir.
  - **Cortes de Câmera**: a câmera de cada trecho.
  - **Evento**: chama o Custom Event do Blueprint do ator vinculado; sem vínculo, chama em todo ator de Blueprint que
    tenha o evento.
- Empacotar o jogo copia os `.rseq`. O Content Browser mostra o tipo "Level Sequence" e o cria em
  Adicionar > Cinemáticas. O duplo clique abre o Sequencer.

## Tempo de execução
- Nós de Blueprint (categoria Cinematic):
  - **Play Sequence**: Loop, Play Rate.
  - **Stop Sequence**: sem sequência escolhida, para todas.
  - **Is Sequence Playing**.
  - O nó guarda o nome da sequência (`seq_load` acha por nome).
- Até 4 players ao mesmo tempo. `seq_tick` roda dentro de `bp_tick`, depois do movimento e antes dos componentes, então
  os componentes presos seguem os atores animados.
- Os vínculos se resolvem no Play. A câmera do corte é procurada uma vez por corte. Nada é alocado por quadro.
- No fim, a sequência segura o último quadro (o "Keep State" da Unreal). Com Loop, os eventos dos dois trechos disparam.
- Enquanto a sequência toca, os Cortes de Câmera dão a vista no PIE e no jogo (`bp_player_camera` consulta `seq_view`).
- `bp_end_play` para todos os players.

## Editor (`src/sequencer_ui.c`)
- **Barra**:
  - Salvar (Ctrl+S).
  - **+ Trilha**: as trilhas do ator selecionado (só as que valem para ele: luz, câmera, Skeletal Mesh), Cortes de
    Câmera e Evento.
  - **Adicionar Chave** (S).
  - Prévia: |<, ▶/❚❚ (Espaço), >|, Repetir.
  - Tempo e quadro, Comprimento, **Travar nos Cortes de Câmera**.
- **Linha do tempo**:
  - Régua em segundos; Ctrl+roda dá zoom.
  - Playhead vermelho (clique ou arraste na régua); barra vermelha no fim do range.
  - Chaves em losango: clique seleciona, arrastar move pelos quadros, Del apaga.
  - Clique duplo na linha cria uma chave com o ator como está.
  - Um ator que falta no nível aparece em vermelho. Botão direito no nome da trilha a apaga.
- **Detalhes da chave**: tempo, valores, "Pegar do Ator", nome (câmera, animação, evento).
- **Prévia**: mover o playhead avalia a sequência nos atores do nível. O estado antes da primeira prévia é guardado e
  volta quando a janela fecha ou o Play começa (o "Restore State" da Unreal). Então editar a cinemática não muda o
  nível salvo.

## Teste e medidas (RTX 3060, 2560×1417)
- Teste `sequencer` (`--seq-test`, `src/seq_test.c`):
  - Cria a sequência: cubo de x 0 a 400 com yaw 0 → 90 em 2 s, corte para SeqCam, evento SeqPing a 1 s.
  - Salva, relê e toca pelo nó Play Sequence de um Blueprint no PIE.
  - Confere: cubo em 400/90, `Pings = 1` pelo Custom Event, sequência terminada, a vista vinda do corte em 1305 quadros.
- Medidas: 578 fps; `seq_tick` < 0,001 ms (3 trilhas).

## Movie Render Queue e Take Recorder (B9, 09/11)
- `src/movie_render.c/.h`, Janela > Cinematics.
- **Fade**: trilha mestra `SEQT_FADE` (v[0] 0..1 linear entre as chaves, v[1..3] a cor, preto por padrão) ->
  `render_set_fade` -> `GpuFrame.fade` (+16 bytes) -> a última linha do `composite.frag`. `seq_end_play` zera.
- **Movie Render Queue**: entra no PIE, aquece N quadros (120: a exposição automática se ajusta; com 20 a imagem saía
  estourada), toca a sequência e passa o relógio fixo 1 / (fps x amostras) ao `main.c` (o `dt` do quadro, seja qual for o
  tempo real). Cada quadro pede `vkctx_request_grab` do retângulo do viewport (no espaço da janela: a posição do viewport
  principal do ImGui descontada), lê no fim do mesmo quadro (a espera da GPU já existia para as capturas) e soma num buffer
  float; a cada N amostras grava `frame_NNNN.png` com a média. Durante a renderização: `game_view` ligado e as dicas e a
  borda do PIE escondidas.
- **Take Recorder**: no PIE, `seq_create("/Content/Cinematics/Takes", "Take")` (a pasta criada), uma trilha Transform por
  fonte (o pawn do jogador e a seleção, pelo nome do ator), uma chave a cada 1/fps exato do take (não por quadro do
  editor); Stop grava o comprimento e salva.
- **Teste** `cinematics` (`--cine-test <pasta>`): um cubo com física cai; gravado 1,3 s a 30 fps (39-40 chaves); o take
  ganha corte para MovieCam e Fade 1 -> 0 em 0,4 s; física desligada (o take o move); 32 quadros a 24 fps x 4 amostras
  em ~2,7-2,9 s; confere todos os quadros, o primeiro escuro (6,6) contra o último (101) e o movimento entre quadros.
- Achado: a Sky Light herda a intensidade padrão (1500) se criada pelo ECS sem o spawn do editor; a dela é um
  multiplicador do céu (1).

## Falta
- Curvas editáveis (tangentes à mão), subsequências/shots, Level Sequence Actor com Auto Play (os componentes chegaram
  a 64: um ator próprio precisa de máscaras maiores, ou uma classe Blueprint da engine), EXR e vídeo na saída, path
  tracing no Movie Render Queue, resolução própria (hoje a do viewport), gravar câmeras e animações no take.

## O Sequencer da Unreal (2026-11-10, "faz o sequencer de cinematics ser igual o da Unreal")
- **Formato v2** (`.rseq`; v1 abre igual): cada chave tem **interpolação** (`SeqInterp`: Auto = cúbica com as tangentes
  dos vizinhos, User = cúbica com as tangentes próprias `tan[9]`, Linear, Constante) e a sequência tem o **início do
  intervalo** (`start`; os players começam ali e voltam para ali no loop). Nas v1, Luz, FOV e Fade abrem Lineares (era
  o que tocavam). `seq_channel_at` é a avaliação de um canal (a mesma do jogo e do Editor de Curvas).
- **Janela** (`sequencer_ui.c` reescrito no layout da Unreal):
  - barra: Salvar, + Trilha (Ator para o Sequencer: qualquer ator do nível; trilhas mestre), Chave Automática (o
    ponto vermelho), Criar Câmera, Travar nos Cortes, taxa de exibição (24..120 fps), Encaixar, Quadros/segundos;
  - transporte: início, chave anterior, tocar para trás, tocar/pausar, próxima chave, fim, repetir, o tempo (Home, End,
    Espaço, Esquerda/Direita);
  - árvore: busca; Cortes de Câmera; cada ator vinculado como grupo (+ Trilha dele; botão direito revincula ao ator
    selecionado ou remove); as trilhas com ◀ ◆ ▶; a Transformação abre em 9 canais com o valor no cursor (editar cria a
    chave); trilhas mestre no fim; ator arrastado do Organizador vira vínculo;
  - linha do tempo: régua em quadros com passo que se ajusta ao zoom, intervalo (início verde e fim vermelho
    arrastáveis), seções por tipo (clipes de animação com o comprimento do clipe, cortes com o nome da câmera, sons,
    trechos visíveis, o trecho das curvas), chaves com forma pela interpolação, seleção múltipla (clique, Ctrl/Shift,
    caixa, chave do grupo = todas do ator naquele tempo), arrastar em grupo pelos quadros, menu de interpolação,
    Ctrl+C/V, Del, duplo clique cria, roda rola, Ctrl+roda dá zoom no mouse, botão do meio arrasta;
  - embaixo: **Detalhes** (quadro, interpolação, valores; nada selecionado: início, fim e taxa) e **Editor de Curvas**
    (canais da trilha, chaves arrastadas em tempo e valor, Shift só o valor, alças das tangentes: pegar uma deixa a chave
    User com a inclinação Auto que tinha; F enquadra; roda dá zoom, botão do meio arrasta).
- **Chave Automática**: a prévia guarda o que pôs em cada ator; se o ator mudou depois (o gizmo), grava a chave no cursor.
- **Criar Câmera** (`editor_spawn_camera`): câmera onde o viewport olha, trilhas Transformação e FOV, corte no cursor,
  Travar nos Cortes ligado.
- Teste `sequencer`: FOV da SeqCam com Constante, Linear, User e Auto, relido do arquivo: 70 / 70 / 90 / 50 nos pontos
  conferidos. `REALENGINE_SEQ_SHOT=curves` abre a aba de curvas com a primeira trilha (capturas).
- **Desfazer** (Ctrl+Z, Ctrl+Y ou Ctrl+Shift+Z na janela): cópias da sequência a cada mudança (48 passos; um arrasto
  inteiro é um passo). Com a janela em foco os atalhos do editor esperam (antes Ctrl+C/V e Del também copiavam, colavam
  e apagavam o ator selecionado no nível). Autoteste `seq_editor_selftest` no teste `sequencer`.
- Falta: subsequências e shots, Level Sequence Actor com Auto Play, tangentes quebradas (entrada e saída diferentes),
  miniaturas nos cortes.
