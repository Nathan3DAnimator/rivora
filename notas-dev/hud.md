# HUD modular (módulo 10 do R7)

Pedido (plano de melhorias, R7 módulo 10): o HUD do jogo montado a partir de widgets reutilizáveis, feito do zero (nada
das demos), configurável no Designer e ligável por Blueprint e scripts. Genérico de propósito: Missões (R7.7, o
`WBP_QuestTracker` e o marcador de objetivo), Game Mode (R7.8, `WBP_MatchHUD`) e Habilidades (R7.9, `WBP_AbilityBar`)
podem usar as mesmas peças e os mesmos nós.

Código: `src/hud.h/.c` (dados, regras, Play, WBP_GameHUD, Debug Viewer, autoteste — no `re_render`, sem ImGui),
`src/hud_widget.c` (o desenho de cada peça, os números de dano, os Detalhes do Designer e do componente — editor e jogo),
`src/hud_test.c` (teste jogado). Integração: tipos novos no fim do enum do UMG (`ui_widget.h`), `UiLive.hud_hidden`,
`ui_live_widget`; o `ui_widget_draw.c` desenha as peças (`hud_widget_draw`), usa o retângulo e a escala do layout
(`live_rect`), a paleta ganhou a categoria HUD; o componente `CHudLayout` (`components.h`, `ecs.c`); nós em
`bp_internal.h`/`blueprint.c`/`bp_vm.c`; a Vida (`health.c`) avisa o HUD de cada dano e ganhou `health_push_number`.

## As peças (widgets nativos do UMG: Paleta > HUD)
Os campos do `UiWidget` são reaproveitados (nenhum campo novo no `.rbp`); os Detalhes mostram os nomes certos.
| Peça | O que faz | Campos |
|---|---|---|
| **Barra de Atributo** | o atributo de um ator (vazio: o jogador): `Health`/`Shield` da Vida, uma fonte registrada (`hud_add_attribute_source`: Stamina, Mana das Habilidades), ou uma variável float do Blueprint (o máximo: `Max<Nome>`, `<Nome>Max`, `Max <Nome>`, senão o Máximo da barra). A perda segue suavizada (Suavização, s), o **trecho perdido** espera (Espera, s) e escoa (fração/s); a cura aparece na Cor do Ganho e o preenchimento alcança; segmentos, números "60 / 100", pulso abaixo de 25%, direção (→ ← ↑). Sem o atributo no ator: some no Play | text = ator, hint = atributo, value/vmin/padding = suavização/espera/escoamento, vstep = pulso, vmax = máximo, columns = segmentos, hover/press = perdido/ganho |
| **Bússola** | faixa com N, NE, L... (pt/en/es, pelo idioma do jogo) girando com a vista, riscos a cada 15°, os marcadores de bússola na direção deles (presos na ponta quando fora da faixa) com a distância | value = graus mostrados |
| **Mensagens** | fila de até 32; **Máximo Mostrado** de cada vez (as outras esperam a vez), **Duração**; o mesmo texto se empilha: `+1 Moeda` + `+1 Moeda` = `+2 Moeda` (soma o número), `Salvo` duas vezes = `Salvo x2`; entra deslizando, some esmaecendo | columns, value, orient (a mais nova em cima/embaixo) |
| **Números de Dano** | desenha os números da Vida (`health.h`, **um buffer só**) nas cores do widget: dano, escudo, cura `+N` em verde e o **crítico** (maior, `!`, com pulo maior). Sem esse widget na tela os números aparecem como antes, nas cores da Vida (`ui_draw_damage_numbers` chama o mesmo desenho) | color/bg/hover/press = dano/escudo/cura/crítico, value = subida (cm) |
| **Mira** | quatro linhas e ponto; abre com o `Set Crosshair Spread` (0-1, suavizado: abre em 0,05 s, fecha em 0,18 s) e com a velocidade do jogador (correr, pular) | value/vmin/vmax = folga/comprimento/abertura máxima, border_w = espessura |
| **Marcadores na Tela** | losango, rótulo e distância sobre o ator (+ altura); fora da vista (ou atrás) ficam na **borda** na direção dele, com uma seta | value = margem da borda, checked = fora da tela |
| **Direção do Dano** | cada dano no jogador (de 16, os últimos) vira um arco em volta do centro na direção de quem causou (o Damage Causer, senão o Instigator), esmaecendo | value = tempo, vmin = raio |
| **Minimapa** | a vista de cima em volta do jogador (alcance em cm): os marcadores de bússola/minimapa (na borda quando longe), os atores com Vida (pontos), o norte, a seta do jogador; gira com a vista ou norte para cima; redondo ou quadrado | value = alcance, checked = girar |
O norte é +X e o leste +Y (os eixos da Unreal); o leste é lido da própria vista (`right`), então vale qualquer mão do sistema.

## HUD Layout (componente `CHudLayout`, 56 bytes: cabe nos padrões de uma classe)
- Details > Adicionar > **HUD Layout** (no Blueprint: Jogabilidade). No Play (depois dos BeginPlay) cria e mostra o widget
  (padrão **WBP_GameHUD**, feito quando falta em Content/UI com as 10 peças: marcadores e números em tela cheia, direção
  do dano e mira no centro, bússola no topo, minimapa e mensagens à direita, vida/escudo/stamina embaixo à esquerda).
- **Escalar Por**: altura da tela (a do UMG), lado menor (celular em pé), caber em 1920 × 1080 (ultrawide), pixels fixos;
  × **Escala do HUD**, com limites. **Margens Seguras** em % (overscan); **Área Segura do Aparelho**: no Android 4% nas
  laterais, 2% em cima, 3% embaixo (o entalhe e os cantos; ainda não lê o recorte real do aparelho). Só o widget do layout
  usa o retângulo e a escala (`hud_layout_rect`).
- **Esconder no Modo de Foto** (`Set Photo Mode`) e **nas Sequências** (`seq_view`: Camera Cuts com a vista); `Set HUD
  Visible` esconde tudo. Escondido, o widget sai da lista de desenho (`UiLive.hud_hidden`, não o Remove from Parent: volta
  como estava) e as peças em qualquer outro widget também somem.
- **Peças Mostradas**: liga e desliga cada tipo no widget do layout (o minimapa começa desligado).

## Nós (User Interface > HUD; nos scripts com o mesmo nome)
Show HUD Message (Text, Duration, Color → bool), Add Compass Marker (Target, Label, Color, Show Distance → Marker Id;
também no minimapa), Remove Compass Marker, Add Screen Marker (Target, Label, Color, Height Offset, Show Off Screen, Show
Distance → Marker Id), Remove Screen Marker, Show Damage Number (Location, Amount, Critical, Heal), Set HUD Visible, Bind
Attribute Bar (Widget — vazio: o widget na tela que tem a barra —, Bar Name, Actor, Attribute → bool), Set Crosshair
Spread, Set Photo Mode, Is HUD Visible. Target solto = o próprio ator. Um marcador por ator e tipo (o novo substitui),
some quando o ator é destruído; até 256.

## Sem malloc no quadro
Tudo em arrays fixos: 256 marcadores, 32 mensagens, 16 danos, 64 estados de barra (por widget vivo e índice), o cache das
larguras dos textos dos marcadores (por slot). As fontes: tamanhos acima de 20 px arredondados para pares (o ImGui 1.92
assa uma fonte por tamanho; os números mudam de tamanho todo quadro).

## Medidas (RTX 3060, 2560 × 1417, Release; 6 sessões dividindo a máquina)
| Caso | Resultado |
|---|---|
| Teste jogado: o HUD inteiro (10 peças) com **102 marcadores** (tela + borda + bússola, metade fora da vista) e **50 números de dano** | **CPU 0,14–0,17 ms** por quadro (mediana; média 0,16–0,18), ~10 mil vértices |
| GPU: a passada de UI do editor (timestamps próprios no `main.c`) com o HUD × sem | **+0,010 ms** (0,032 × 0,022 ms) |
| Por peça (um quadro): Marcadores na Tela 0,075–0,09 ms (5,8 mil vértices), Bússola 0,023–0,03, Números 0,028, Minimapa 0,015, Barras 0,007–0,012, Mira 0,002 | |
| Antes das otimizações (contorno por traço, negrito em todo texto, larguras medidas todo quadro, tamanhos livres) | CPU 0,18–0,23 ms, 12,6 mil vértices; os números 0,062 ms |
| `hud_tick` com 100 marcadores | 0,001–0,003 ms |
| Cena do teste com validação | 375–544 fps (meta 200) |
Meta: < 0,2 ms (o teste cobra a mediana de CPU e a diferença de GPU). O Debug Viewer (F10) tem a lista **HUD**: update,
desenho (CPU, vértices, a passada de UI na GPU), cada tipo de peça e cada marcador.

## Teste `hud` (`--hud-test`)
1. `hud_selftest`: marcadores (um por ator e tipo, remover por tipo e por id, lugar fixo, cheio em 256, ator destruído),
   mensagens (fila de 5 de 7, `+1`+`+1`+`+3` = `+5 Coin`, `Saved x2`, todas mostradas e somem em 20 s), a barra (perda
   suavizada, trecho perdido que espera e escoa, a cura no ganho), atributos, rumo (+X 0°, +Y 90°...), projeção (centro,
   borda, atrás), direção do dano (só o do jogador), visibilidade e modo de foto, a mira suavizando; o tick cronometrado.
2. Jogado (projeto novo): BP_HudPlayer (Character com Vida, HUD Layout com todas as peças e Stamina 40 de MaxStamina 80);
   BP_HudTarget com BeginPlay chamando Add Screen Marker, Add Compass Marker, Show HUD Message `+1 Coin` duas vezes e Show
   Damage Number crítico. Confere: o WBP_GameHUD na tela, `+2 Coin`, os dois marcadores do alvo e ele à frente na tela, o
   77 crítico no buffer, a Stamina em 0,5 de 80 (variável do Blueprint), 40 de dano: barra em 0,6 com o trecho em 1 aos
   0,3 s e escoado aos 1,7 s, o arco vindo do leste, Bind Attribute Bar (75 de 150 do alvo; uma barra que não existe é
   recusada), a mira aberta, margens 2% e escala 1,5 (retângulo e escala certos), Set HUD Visible e o modo de foto
   escondendo e voltando. Por fim mede com 102 marcadores e 50 números, alternando HUD mostrado/escondido a cada 40 quadros.

## Limites / próximos passos
- A área segura do Android é fixa (sem `DisplayCutout` real); o modo de foto é só a flag (não há câmera livre de foto).
- O esconder nas Sequências não tem teste próprio (usa o `seq_view` que o Sequencer já testa).
- O minimapa não desenha o cenário (só marcadores, atores com Vida e o jogador): uma vista de cima renderizada fica para
  quando houver render target em widget.
- Marcadores com ícone (textura) e barra sobre a cabeça de inimigos (widget no mundo) ficam para o Widget Component.
