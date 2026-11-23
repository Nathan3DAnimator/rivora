# Interação (módulo 5 do R7)

Código: `src/interaction.h/.c` (componentes, foco, teclas, segurar, condição, dispositivos, WBP_InteractPrompt, Debug
Viewer, autoteste — no `re_render`), `src/interaction_ui.c` (Details do nível e dos componentes de um Blueprint),
`src/interaction_test.c` (teste jogado), nós em `bp_internal.h`/`blueprint.c`/`bp_vm.c`, eventos nos scripts em
`script_lower.c`, destaque em `render.c` + `shaders/mask.frag` + `shaders/composite.frag`. Feito do zero e modular: não
usa o "E interagir" da demo de terror nem o do Pickup do inventário.

## Componentes (components.h, cada um ≤ 128 bytes: cabem nos padrões de uma classe Blueprint)
- **Interactable** (`CInteractable`, 120 B; Details > Adicionar > Interagível): Texto do Aviso (`Abrir {0}`, `{0}` =
  Nome de Exibição ou o nome do ator; vazio: o verbo do dispositivo — Abrir/Fechar, Puxar, Apertar — ou Interagir),
  Alcance e Ângulo de Visão (0: os do Interator), Tecla (Padrão: a do Interator) ou Ação de Entrada (Enhanced Input),
  Tempo Segurando (0: um toque), Pode Repetir (desligado: `IA_ONCE`), Habilitado, Prioridade, Condição: Tag Exigida
  (Gameplay Tag) num item do Inventário (`inv_find_tag`) e/ou nas Gameplay Tags de quem usa (`gt_has`), Gastar o Item.
  Cumprida uma vez, fica destrancado (`IA_UNLOCKED`, só do Play).
- **Interactor** (`CInteractor`, 52 B): alcance 250 cm, ângulo 30°, tecla E (sem ação: também o X do controle),
  Contorno / Brilho / Mostrar Aviso / Precisa Ver; os campos do Play (em foco, segurando, progresso). **O pawn possuído
  ganha um sozinho** (`ecs_add` no começo do Play e quando outro é possuído; o snapshot do PIE desfaz).
- **Interactive Device** (`CDevice`, 112 B): Porta, Baú, Alavanca, Botão; Movimento (Girar na dobradiça — eixo Z na
  borda −X ou +X —, Deslizar ao longo de X, Inclinar — eixo Y na borda de trás embaixo, a alavanca no pé), ângulo ou
  distância, tempo, fechar sozinho, começa aberto, abre para longe de quem abre; Parte Móvel (outro ator: a tampa do
  baú; vazio: ele mesmo); Ator Alvo e Evento Personalizado (alavanca/botão); 3 itens do baú.

## Como funciona (`interaction_tick`, no `bp_tick` depois do `inv_tick`)
- **Dispositivos**: `t` 0..1 anda para aberto/fechado em `time` s (suavizado); a pose da parte móvel é a fechada (lida no
  começo do Play) girada em torno do pivô na malha (`render_mesh_bounds` + `render_transform_rows`: o pivô fica parado),
  ou deslizada. Animação simples em vez do Physics Constraint: não depende de massa nem de assentar, abre sempre igual e
  não empurra o jogador por acidente. As partes móveis e os alvos ficam **Movable** no começo do Play (antes do
  `build_boxes`): a caixa de colisão delas segue (`track_boxes`).
- **Foco**: para cada Interator, a visão (o jogador: `bp_player_camera`; outro: o olho e a rotação do ator) e o olho do
  pawn (alcance e visão medidos dele). Cada Interagível: rejeição rápida pela posição; caixa no mundo; distância do olho
  ao ponto mais perto da caixa ≤ alcance; dentro do cone **ou** o raio da câmera acerta a caixa. Nota = prioridade ×
  1000 + 200 (raio) + 100 × centralizado + 50 × perto; os 4 melhores, e o primeiro que o olho **vê** ganha: um traço
  (`bp_trace_boxes`) até o ponto mais perto da caixa — ou da Parte Móvel, a tampa cobre o baú — chega a 8 cm dele.
  Mudou: On Focus End no antigo, On Focus Begin no novo (eventos podem mexer no mundo: a linha é relida).
- **Tecla** (só a do jogador; outro Interator usa o nó Interact): ação do Enhanced Input (`input_action_state`) se houver,
  senão a tecla (`bp_key_held`) e, sem ação, o X do controle (`input_key_held`). Toque: usa na borda. Segurar: acumula
  enquanto segura o mesmo, On Hold Progress todo quadro, usa ao chegar no tempo e espera soltar; soltou antes: progresso 0.
- **Usar** (`interaction_interact`, também o nó Interact): habilitado, não usado (uma vez só), condição (gasta o item);
  depois o dispositivo: porta abre/fecha; baú dá os itens a quem abriu (`inv_add`; o que não coube fica) e abre/fecha;
  alavanca alterna e **liga e desliga** o alvo; botão entra e volta sozinho (Fechar Sozinha 0,4 s) e alterna o alvo. O
  alvo: com Evento Personalizado, `bp_call_event_args` com a entrada bool `On`; senão um dispositivo abre/fecha, uma luz
  alterna o Affects World (as opções de luz ganham os padrões quando o `ext` era 0), outro ator alterna Hidden In Game
  (some e perde a colisão). Por fim On Interact (Instigator) no ator.
- **Eventos** chegam pelo `bp_fire_actor_event` (o ator e seus Actor Components): On Interact (Instigator), On Focus
  Begin / End (Instigator), On Hold Progress (Progress, Instigator). Nos scripts: `OnInteract(instigator)`,
  `OnFocusBegin`, `OnFocusEnd`, `OnHoldProgress(progress, instigator)`; os nós entram sozinhos na API
  (`Interact`, `GetFocusedInteractable`, `SetInteractionEnabled`, `SetPromptText`, `GetInteractionPrompt`,
  `SetDeviceActive`, `IsDeviceActive`).
- **Destaque**: `interaction_highlight` dá o ator em foco do jogador e a parte móvel; o `RenderView.highlight[2]` entra
  na máscara da seleção do editor com o id 255 (push `pad` 1 no `mask.frag`; a seleção usa 1..254) e o
  `composite.frag` contorna em branco quente (Contorno) e/ou clareia a silhueta (Brilho), `pad0` bits 1-2. Atores sem
  Static Mesh (um Skeletal Mesh) não ganham destaque, só o aviso.
- **WBP_InteractPrompt** (`interaction_prompt_asset`, Content/UI, criado quando falta: ao pôr um Interagível/Interator ou
  um dispositivo, e no Play com Mostrar Aviso): Frame (Canvas) com fundo, a tecla numa caixa clara, o texto e a barra de
  segurar; o Event Tick usa **Get Interaction Prompt** (Get Player): Frame visível só com foco, a barra só segurando.
  A tecla: `gus_action_key` da ação (o remapeamento do jogador vale) ou a tecla; com o controle como último dispositivo
  (`input_gamepad_used`: botões ou sticks; teclado/mouse voltam) vira o botão do controle (A, B, X, Y, LB...).
  Trancado: "Trancada (Chave Vermelha)" — o nome do primeiro Item Definition com a tag, senão a tag.
- **Place Actors / menu rápido > Jogabilidade**: Porta (1 × 2,1 m, 10 cm, 95° em 0,6 s), Baú (60 × 90 × 50 cm + a tampa,
  um segundo ator `<nome>_Lid` como Parte Móvel, 100° em 0,7 s; mova os dois juntos), Alavanca (60 cm inclinada −35°,
  vira +35°), Botão (vermelho a 1,1 m, entra 4 cm por 0,4 s). `add:Door`, `add:Treasure Chest`... no Assistente.
- **Debug Viewer** (F10) > Interaction: o custo do quadro e cada Interagível (em foco, habilitado, usado, precisa de).

## Medidas (RTX 3060, Release)
| O quê | Custo |
|---|---|
| Autoteste: 200 Interagíveis numa grade de 20 × 20 m em volta, o Interator girando (sem traço) | 0,0023 ms/quadro |
| Teste jogado: 207 Interagíveis no nível, 6 dispositivos, o Interator do jogador com traço de visão | 0,0078 ms/quadro |
| Cena do teste (2560 × 1417, mailbox) | 423 fps (2,36 ms) |
Meta: < 0,1 ms/quadro (o teste cobra). O destaque é um desenho a mais na máscara (a passada já existe).

## Teste (`interaction`, ctest: `--interaction-test`, projeto novo `test_projects/IA`)
Autoteste: foco pela visão, alcance e prioridade, desabilitado, tecla uma vez, uma vez só, segurar (0,5 s: progresso;
soltou: 0; 1 s: usa), condição por Gameplay Tag (e fica destrancado), texto `{0}` e a tecla X do controle, porta girando
90° na dobradiça para longe de quem abre, deslizando, fechando sozinha, alavanca abrindo porta / apagando luz / escondendo
ator / chamando evento com On, cabo da alavanca no pé, botão que volta, tampa do baú subindo; 200 cronometrados.
Jogado: olhar +X foca a porta e o aviso mostra "E" e "Abrir"; E abre 95° para longe; a porta trancada (Item.Key.Red)
diz o nome da chave e não abre; com a chave no inventário abre e a chave é gasta; BP_Valve (Blueprint com o Interagível
nos padrões da classe, segurar 1 s): On Focus Begin, On Hold Progress ~0,5 na metade com a barra na tela, On Interact
uma vez em 1 s; um botão atrás de um bloco não é focado e o aviso some; a alavanca abre o portão; o baú dá 2 gemas; com
controle a tecla vira X; custo medido com 207 Interagíveis.
