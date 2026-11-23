# Diálogo (módulo 6 do R7)

Código: `src/dialogue.h/.c` (asset `.rdlg`, regras, a conversa do jogo, widget, câmera, memória e save — no `re_render`),
`src/dialogue_editor.c` (editor do asset com prévia, Details do Dialogue Speaker — só no editor), nós em
`bp_internal.h`/`blueprint.c`/`bp_vm.c`, teste `src/dialogue_test.c`. Feito do zero e modular (não usa o `rpg_say` da
demo de RPG).

## Como funciona
- **Asset Dialogue** (`.rdlg`, JSON; Content Browser > Adicionar > Diálogo, vazio ou o **Exemplo do Mercador**): grafo de
  até 128 nós. Tipos: **Início** (o nó 0, sempre), **Fala** (quem fala: vazio = o NPC, `Player` = o jogador, ou um nome;
  texto em inglês/português/espanhol; voz — Sound Wave ou Cue —; animação — Montage, senão Sequence — tocada em quem fala;
  esperar o jogador ou Duração, 0 = a voz acabar / 2 s), **Escolha** (as Opções ligadas a ela; sem texto, a última fala
  fica na tela: a pergunta), **Opção** (o que o jogador diz; **Escolher Só Uma Vez**), **Saltar para o Nó** (vai para outro nó por
  índice, sem fio de volta) e **Fim**. Cada nó tem até 8 ligações, 3 condições e 3 ações.
- **Regra de caminho**: ao sair de um nó, segue a **primeira ligação cujo nó passa nas condições** (a ordem é a das
  ligações, numerada no canvas); nenhuma passa = fim. A Escolha mostra **cada Opção que passa** (condição falha esconde a
  opção; Escolher Só Uma Vez já escolhida também); sem nenhuma, segue as outras ligações ou termina. Até 64 nós por passo (laço
  de saltos sem fala termina em vez de travar).
- **Condições** (do jogador ou de quem fala, com "Não"): Tem a Gameplay Tag (hierárquica: `Quest` cobre
  `Quest.Merchant.Bought`), Tem o Item (Inventário, pelo nome do Item Definition, quantidade mínima), Variável (do
  Blueprint, comparada com >= > == != < <=), Vida (percentual do componente Health), Já Dito (um nó já passado).
- **Ações ao passar** (em ordem): Dar Item / Tirar Item (Inventário), Pôr / Tirar Gameplay Tag, Chamar Custom Event com
  até 2 entradas (texto convertido para o tipo de cada entrada do evento: número, bool, vetor "x y z", texto —
  `bp_call_event_text`), Definir Variável, Abrir a Loja (o Widget Blueprint nomeado vai para a tela com o cursor, e o
  Custom Event `OpenShop` de quem fala roda — a loja de verdade é de outro módulo).
- **Idiomas**: o texto em inglês é o msgid. Português e espanhol têm campo próprio; vazio = o inglês, e em português a
  tradução da tabela da engine (`tr_lang`: "Goodbye" vira "Tchau" sozinho). O idioma é o do jogo (Opções > Idioma); como a
  engine só tem en/pt, o espanhol vem de **Set Dialogue Language** ("es") ou do Windows em espanhol quando o jogo segue o
  sistema (`GetUserDefaultUILanguage`).
- **Dialogue Speaker** (`CDialogueSpeaker`, 80 bytes, cabe numa classe Blueprint): o Diálogo, o nome exibido, pausar o
  jogo (como o Diálogo diz / sempre / nunca), virar para o jogador, a **câmera de conversa** (distância, altura, ângulo:
  ao lado do jogador olhando o rosto do NPC, entra em 0,5 s com smoothstep a partir da câmera do jogo; entra no
  `bp_player_camera` logo depois da Level Sequence) e um widget próprio (vazio: WBP_Dialogue).
- **Nós** (categoria Dialogue): Start Dialogue (Target = o NPC; Dialogue pelo nome ou o do Speaker; Player desligado = o
  pawn possuído), Choose Option (índice entre as visíveis), Skip Line (mostra o texto todo; já todo, avança), End
  Dialogue, Is In Dialogue, Set Dialogue Language. Eventos (para todos os Blueprints que os têm, como os de rede):
  On Dialogue Line (Speaker, Speaker Name, Text, Line), On Dialogue Choice (Speaker, Option, Text), On Dialogue End
  (Speaker, Dialogue). Nos scripts: `StartDialogue(...)` e os métodos `OnDialogueLine/Choice/End` (`script_lower.c`).
- **WBP_Dialogue** (Content/UI, criado ao adicionar um Dialogue Speaker ou um Diálogo): Border embaixo com Speaker, Line
  (letra a letra: o módulo põe o prefixo UTF-8 com `ui_set_text` só quando muda) e Choices (Option1..6, Buttons com
  OptionNText); um Button transparente **Continue** cobre a caixa. O gráfico do próprio widget faz
  `OnClicked_Continue -> Skip Line` e `OnClicked_OptionN -> Choose Option(N-1)` — um WBP do usuário com os mesmos nomes
  funciona igual. O cursor aparece durante a conversa, então a navegação do UMG vale: setas/direcional escolhem, Enter,
  Espaço ou o A do controle confirmam. O widget é reaproveitado entre conversas (sem entidade nova a cada uma).
- **Pausa**: com pausa, `game_set_paused(true)` (se o jogo não estava pausado); o `dlg_tick` roda dentro do `bp_tick`
  antes do retorno da pausa, então o texto e o widget seguem; a voz toca como som de UI.
- **Memória e save**: o que já foi dito/escolhido fica num bitset de 128 bits por Diálogo (192 Diálogos), usado por Já
  Dito e Só Uma Vez. **Save Game to Slot** grava como mais um registro (`#Dialogue`) do fluxo do save, depois do inventário (e o Save Progress, num chunk `DLGM` próprio; New Game limpa);
  o Load lê de volta. Um save sem variáveis do ator ainda grava a memória.
- **Prévia no editor**: roda a mesma regra (`DlgPreview`) sobre um jogador de mentira — tags, itens (nome e quantidade),
  vida, variáveis — que as condições leem e as ações mudam; mostra o nó atual com borda verde no canvas e um log do que
  aconteceu (ações, eventos). Idioma escolhido na prévia vale para o canvas.
- **Integração com a Interação**: ainda não está na main (sessão paralela). O ponto de entrada é
  `dlg_start_speaker(w, npc, "", player)` — o prompt "Falar" do módulo de Interação deve chamar isso no ator com
  Dialogue Speaker. Até lá, Start Dialogue num evento (tecla, overlap).

## Medidas (RTX 3060, 2560x1417, com validação; `--dialogue-test`, isolado)
| Medida | Valor |
|---|---|
| `dlg_tick` com uma conversa na tela (letras aparecendo, widget) | 0,0006–0,0008 ms/quadro |
| `dlg_tick` sem conversa | um `if` (retorna na hora) |
| Start Dialogue (widget já existente) | 6,6–7,7 µs |
| Start Dialogue na primeira vez (cria o widget) | 77 µs |
| Choose Option + Jump + evento no Blueprint | 1,0–1,4 µs |
| Todos os widgets desenhados (com o WBP_Dialogue) | 0,012 ms |
| Teste inteiro (PIE com a conversa e a janela do editor do Diálogo abertas) | 620–654 fps |

Antes/depois: fora de uma conversa o custo por quadro é zero (o módulo não percorre o mundo); a medida dentro dela está
na tabela. Com outras sessões usando a GPU a medida caiu para 184–194 fps (pior quadro 145 ms) e voltou a 620+ isolada.

## Teste (`ctest -R dialogue`)
`dlg_selftest` (a regra pela prévia: textos nos 3 idiomas, letras por segundo, Skip, opção escondida, Só Uma Vez, salto,
compra com moeda, tag, fala com duração, registro do save) e depois, no Play: Start Dialogue pelo nó (Custom Event Talk
do NPC), letras aparecendo e o Line do widget igual ao prefixo, clique no Continue (texto todo, depois a Escolha), 3 de
4 opções (a do segredo escondida pela tag), troca de idioma para português (texto próprio e "Goodbye" pela tabela) e
espanhol, Choose Option no "de novo" com o salto de volta e o evento On Dialogue Choice, Enter pela navegação do UMG na
opção de compra (tira a moeda, dá a poção, põe `Quest.Merchant.Bought`), Save/Load Game to Slot da memória, fim com o
Custom Event `OnDlgDone(42)` e On Dialogue End, pausa ligada/desligada, câmera de conversa; depois a medida com a
janela do editor e a prévia abertas.

## Limites / próximos passos
- Uma conversa por vez (a do jogador); conversas de NPC com NPC ficam para depois (latido / barks).
- Texto até 200 bytes por idioma (o Text do UMG passou a 256 bytes por isso); 6 botões de opção no WBP_Dialogue.
- O texto de uma Fala não tem variáveis (`{PlayerName}`) nem rich text.
- O UMG traduz sozinho um texto que seja msgid da tabela (em inglês forçado com o jogo em português, "Goodbye" aparece
  "Tchau").
