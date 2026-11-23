# Kit de terror (Q13, plano 0.43, 2026-11-18)

O que a análise do curta de terror pediu (plano 0.43, seção 2: brilho médio Y 32/255, 80% dos quadros abaixo de Y 40,
planos de 2,3 s, flashes de 1-2 quadros, letterbox ~2,2:1, luz colorida motivada, recorte/silhueta, olhos brilhando no
preto, P&B estourado, alucinação, DOF raso, Dutch, fades para preto), como ferramentas da engine. Código em
`src/terror.c/.h` (o núcleo, thread do jogo, nada alocado por quadro), `shaders/composite.frag` (a pilha de filtros),
`shaders/exposure.comp` (a exposição de terror), `src/lights.c` e `shaders/mesh.frag` (canais, cintilação, olhos).
Artigo do Assistente: `assistant/articles/horror-kit.md`.

## Diretor de Tensão
- Um valor 0-1 (**Set Tension** com Blend Time, **Add Tension**, **Get Tension**). Sem tempo (-1) sobe e desce pelas taxas
  do volume (Subida 0,5/s, Descida 0,25/s por padrão). Volta a 0 no começo e no fim do Play (`terror_reset`).
- Canais (curvas de 4 pontos: tensão 0, 1/3, 2/3, 1; Catmull-Rom sem passar dos pontos): grão, vinheta, saturação,
  letterbox, FOV (x), pulsação (batimento de 60 a 150 bpm, "lub-dub", escurece as bordas), cintilação das luzes com
  **Cintilação pela Tensão**, volume e passa-baixa de uma **Sound Class** (os valores dela guardados e devolvidos no fim)
  e uma entrada de **MetaSound** em todo Audio Component tocando.
- As curvas ficam no Post Process Volume > **Terror** > Curvas de Tensão (o volume de maior prioridade na câmera com o
  grupo ligado; sem ele, as padrão de `g_tension_channels`).

## Pilha de filtros (composite.frag)
- Sete filtros, cada um com o próprio valor 0-1 = o maior entre o evento (**Trigger Horror Filter**: entra, segura, sai),
  o valor segurado (**Set Horror Filter** com esmaecimento; **Clear Horror Filters**) e o da trilha do Sequencer
  (**Filtro de Terror**: cada chave o filtro ou "Letterbox" e a intensidade, linear até a próxima; uma trilha por
  filtro ao mesmo tempo, mais trilhas para empilhar).
  - **Alucinação**: células de ~6 px (1440p) de ruído colorido, monocromático vermelho, posterizado em 4 níveis, pretos
    esmagados e brancos estourados.
  - **P&B Estourado**: 1 - e^(-7L - 0,45) em cinza: até o preto vira cinza (o "snap" de ~1 s do vídeo).
  - **Flash Branco**, **Estática de TV** (chuvisco, linhas, barra rolando), **VHS** (linhas empurradas, faixa de tracking,
    croma borrado de lado, linhas de varredura, cor lavada), **Aberração Cromática** (soma à da lente), **Distorção**
    (onda e barril que respira: mexe onde a imagem é lida).
- Depois deles o **Dip to Black / Dip to White** e as barras do **letterbox** (Set Letterbox até uma proporção, a curva
  de tensão ou a trilha; a maior das três), nas linhas sem distorção.
- O ruído segue o **relógio do kit** (24 passos por segundo), não o número do quadro: o mesmo instante dá a mesma imagem
  (o teste segura o relógio e compara). Um hash por pixel (seus bytes são o grão, a fita e o chuvisco), um por célula,
  um por linha.
- O pé da curva e a pulsação/vinheta/saturação/grão da tensão rodam no mesmo bloco, depois do tone mapping e do grão.
  Tudo só roda com `terror[3].w` (algo ligado): fora do terror o composite não muda (um teste uniforme por pixel).

## Exposição de terror (Post Process Volume > Terror)
- **Exposição de Terror**: a medição conta o preto (bin 0) e tira a média da faixa de 15% abaixo da **Parte Medida**
  (0,95: o vigésimo mais claro), alvo 1 stop acima do cinza médio. Um quarto escuro com uma lâmpada fica escuro e a
  lâmpada exposta; a média comum levanta o quarto para cinza.
- **Piso de EV**: o Min EV100 da exposição automática nunca abaixo dele (`render.c`, só com o grupo ligado).
- **Pé da Curva**: c' = c²(1+k)/(c+k), k = 0,3·pé: 1 continua 1, nada sobe, as sombras vão para o preto.

## Luzes
- **Cintilação da Luz** (campos no fim de `CLight`, sem componente novo): Vela (ruído suave + tremor + "engasgo"),
  Lâmpada Defeituosa (rajadas de 25 Hz em 35% das janelas de 1,7 s), Estrobo (8 Hz, 15% aceso), Relâmpago (um raio em
  65% das janelas de 7 s, 2-3 flashes com retornos), Pulsação (farol). Intensidade e velocidade. Determinístico pelo
  relógio e pela entidade (`terror_light_factor`, chamado em `lights_gpu`: a luz entra no conjunto que é comparado todo
  quadro). **Som da Cintilação** tocado na luz a cada raio/rajada/flash, com atraso (o trovão).
- **Luz de Recorte** (Rim Light): atrás do jogador ou de um ator pelo nome, longe da câmera no plano, acima e de lado,
  mirando no peito; vira Movable. **Set Rim Light Target**.
- **Canais de Iluminação** (Lighting Channels 0-2, na luz e na malha; **Set Lighting Channels** passa aos componentes):
  bits 4-6 do `GpuLight.type` e 13-15 do `INST_` (o canal 0 invertido: 0 = só o canal 0, o padrão, níveis antigos
  intactos). Testado em `shade_light` e no alvo do ReSTIR (`ml_target`). Só a luz direta: o rebatimento da GI e o
  cabelo/tecido/partículas não separam canais (como a Unreal com Lumen).
- **Poeira no Feixe**: bit 7 do tipo; na neblina volumétrica o espalhamento da luz vezes 0,4 + 9·n² (três ruídos 2D
  cruzados, flutuando devagar).
- Correção junto: a Light Function do Q4 testava `g->type != 3u` com os bits de escala já gravados: luzes retangulares
  perdiam o tamanho (viravam quase pontuais). Agora `(type & 7) != 3`.

## Olhos que brilham
- Material > Shading Model Eye > **Brilho dos Olhos** e a cor (e5.w do material: força x16 nos bits altos, RGB565).
  Na íris (r < 0,9) um lóbulo de retro-reflexo no `brdf`: cor · força · (l·v)^40, sem cosseno: com a lanterna ao lado
  da câmera os olhos acendem no escuro. Exemplo `Examples/M_EyeGlow` (o M_Eye com brilho 4, verde-dourado).

## Rigs de susto (o ponto de vista do jogador, `terror_view` em `post_fill_view`)
- **Dolly Zoom** (a lente abre enquanto a câmera avança: o sujeito a 3 m do mesmo tamanho, o fundo estica), **Chicote**
  (55° de lado e volta, o motion blur da câmera borra), **Ângulo Holandês** (18°, segura, solta). Feitos na própria vista:
  o Camera Shake do Q12 não existe neste branch.

## Demo `horror` (a clínica)
- Exposição de terror no volume (piso EV 1, até EV 4 com a energia de volta, pé 0,45), lamparinas com Vela, duas
  lâmpadas do corredor Defeituosas e todas com Cintilação pela Tensão 0,7, a luz verde da saída em Pulsação, a lanterna
  com Poeira no Feixe.
- A criatura: Luz de Recorte fria (ciano: perigo) no canal 1 que só a ilumina (ela nos canais 0 e 1), olhos M_EyeGlow.
- O jogo: a tensão pela distância da criatura (piso 0,3 com a energia), chicote + aberração quando passa de 0,75; pego:
  Flash Branco, Alucinação, Holandês e Dip to Black; o relógio às 3: P&B Estourado; a energia volta: letterbox 2,2:1,
  dolly zoom e +0,4 de tensão; abre do preto entre barras; a saída: Dip to White.

## Testes (área `render`)
- `terror_filters`: cada filtro inteiro com o relógio segurado, duas leituras do viewport a 5 quadros: a mesma imagem
  (Estática de TV: 0 de diferença; os outros < 0,006, o ruído da própria cena); a cara de cada um (Flash 1,0; P&B
  saturação 0 e mais claro; Alucinação vermelha; Estática sem cor, desvio 0,2, muda com o relógio); o letterbox preto; o
  custo da pilha inteira.
- `terror_exposure`: quarto fechado, uma lâmpada pequena, exposição automática: a parte escura 0,2255 → 0,0115 (sRGB) com
  a de terror; o 1% mais claro 0,925.
- `terror_channels`: luz no canal 1 sobre dois cubos: o do canal 1 0,85 aceso / 0 apagado; o do canal 0 0,0015 / 0 (sem
  vazar: < 2/255); com a luz no canal 0, 0,85.

## Medidas (RTX 3060, viewport 2132x1322 dentro de 2560x1417, Release)
- Pilha inteira (os 7 filtros, tensão 1, letterbox, dip): composite 0,221 → 0,381 ms = **0,159 ms (0,208 ms em 1440p)**
  com a GPU livre; 0,219-0,284 ms em 1440p com outro realengine.exe dividindo a GPU. Meta < 0,3 ms. Com 16 hashes por
  pixel (a primeira versão) era 0,296 ms (0,387 em 1440p).
- Desligado: o composite não muda (o bloco não roda).
- CPU do kit por quadro: no Debug Viewer > Horror Kit (`terror_stats`).
- Demo `horror`: ver "Demo medida" abaixo.

## Demo medida (RTX 3060, 2560x1417)
| Teste | Antes (demo antiga, mesma engine) | Depois (kit inteiro na demo) |
|---|---|---|
| `demo_horror` (Médio, Play com autoplay) | 191 fps, GPU 5,15 ms, composite 0,313 ms | 185-187 fps, GPU 5,24-5,30 ms, composite 0,39 ms |
| `demo_horror_flicker` (Épico, editor) | 125 fps, cintilação 0,051% | 124 fps, cintilação 0,049% |
- O custo novo: o composite com a tensão/letterbox/eventos (+0,08 ms), 3 luzes Movable a mais (as que piscam e o
  recorte) e os olhos. A "cintilação" do `demo_horror` (0,77%, antes 0,046%) é a do Play com autoplay: agora as
  lâmpadas piscam de propósito e a câmera faz dolly zoom; o teste de cintilação de verdade é o `demo_horror_flicker`
  (editor parado, as luzes como configuradas: igual ao antes).

## Falta
- Canais na GI, no cabelo e no tecido; um Camera Shake de verdade nos rigs quando o Q12 entrar; sons de choque prontos.
