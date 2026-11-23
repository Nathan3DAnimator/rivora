# Salvar e carregar o progresso (módulo 4 do R7)

Código: `src/save_progress.h/.c` (formato, slots, progresso, Save Game, checkpoints, auto save, a escrita numa thread —
no `re_render`), `src/save_menu.c` (WBP_SaveSlots e os botões do menu de pausa, no `re_render`), a foto em `vkctx.c`
(`thumb_req`) e `brand.c` (`brand_ui_image_rgb`), nós em `bp_internal.h`/`blueprint.c`/`bp_vm.c`, teste
`src/save_test.c`. Feito do zero e modular: não depende das demos; Vida, Inventário e o menu de pausa (módulos 1 a 3)
entram pelas funções deles.

## Como funciona
- **Save Game** (o USaveGame da Unreal): uma Blueprint Class com pai `Save Game` guarda as variáveis do progresso.
  **Create Save Game Object** cria um (um ator sem lugar no mundo); **Save Game to Slot / Load Game from Slot** gravam e
  leem as variáveis dele (ou de qualquer ator, como antes, mais o Inventário). **Project Settings > Save Game Class** é o
  do progresso: **Get Progress Save Game** o cria na hora, ele passa de nível para nível (as variáveis ficam num blob de
  64 KB no `progress_end_play` e voltam no `progress_begin_play`) e entra em todo Save Progress.
- **Saveable** (`CSaveable`, 16 bytes; Details > Adicionar e componentes da classe): o que o ator guarda — transform,
  Vida (vida, escudo, máximos, morto), Inventário (o registro do `inv_save_append`), variáveis marcadas **SaveGame**
  (`BpVar.save_game`: o checkbox nos Details da variável; `@savegame` no Real/Python, `[SaveGame]` no C#) ou todas,
  oculto no jogo, e **destruído** (Remember Destroyed). Chave no arquivo: `id` (o editor sorteia ao adicionar; muda de
  nome e continua; o Ctrl+D dá outro à cópia) ou o hash do nome; duas iguais (copiar e colar) → a segunda ganha outra, a
  mesma toda vez para o mesmo nível.
- **Destruídos**: no BeginPlay o módulo anota as chaves dos Saveables do nível (tabela hash aberta de 64 Ki); no save,
  as que não estão mais vivas (com Remember Destroyed) vão para o chunk DEST. No load o nível abre de novo do arquivo e
  esses são destruídos **antes do BeginPlay** (sem End Play: nunca começaram). Quem foi destruído depois do save volta.
- **Criados no jogo** (Spawn Actor de uma classe com Saveable): sem chave do nível → gravados com a classe e criados de
  novo no load (onde estavam). O **jogador** (o pawn possuído) sempre tem o próprio registro (os flags do Saveable dele,
  ou o padrão), aplicado no primeiro quadro depois do BeginPlay (o Default Pawn nasce no `project_begin_play`).
- **Load Progress**: lê e confere o slot, pede o **Open Level** do nível do save (`level_open_request`) e, quando ele
  começa (`progress_begin_play`, chamado no `bp_begin_play` entre os Construction Scripts e o BeginPlay), põe de volta
  o progresso (tempo de jogo, checkpoint), o Save Game e os Saveables do nível; no primeiro `progress_update` o jogador e
  os criados, e o evento **On Game Loaded**. Nível que não existe no projeto (um nível do editor nunca salvo): aplica
  em cima do mundo como está. Um save de ator (kind Object, versão 1) no Load Progress vai para o jogador.
- **Slots** (`Saved/SaveGames/<slot>.sav`; `REALENGINE_SAVE_DIR` troca a pasta — os testes): nome do slot limpo (nunca
  um caminho). Cabeçalho fixo de 256 bytes com os metadados, então **List Save Slots** lê só o cabeçalho de cada arquivo
  (o mais novo primeiro). **Get Save Slot Info**, **Does Save Game Exist**, **Delete Game in Slot**, **Get Latest Save
  Slot** (Continuar), **Get Save Slot Name** (índice), **Get Current Save Slot**, **Get Play Time** (não conta pausado).
- **Foto**: o save pede ao `vkctx` uma cópia do próximo quadro (`thumb_req`): o retângulo do jogo (no editor, só o
  viewport: `progress_set_view_rect`), cortado em 16:9 e reduzido para 192×108 (média de até 4×4 amostras), com os
  widgets escondidos naquele quadro (`progress_hide_ui` no `ui_draw_viewport`). Memória HOST_CACHED para a leitura (a
  não-cacheada levava ~1 s para 2560×1440) e amostrada onde está, sem copiar o quadro inteiro. O arquivo é gravado uma
  vez, já com a foto, no quadro seguinte; se algo lê o slot antes (lista, load, info), grava na hora sem ela e a foto
  vem numa segunda escrita. Sem foto em 4 quadros (janela minimizada): grava sem. Na UI: um Image com
  `bind_setting = "Save.N.Thumb"` ou `image = "save:<slot>"` vira textura por `brand_ui_image_rgb` (8 em cache).
- **Escrita**: o jogo calcula o checksum e copia o buffer; a thread de escrita cria a pasta, grava `<slot>.sav.tmp` e o
  troca pelo antigo (`re_replace_file`, write-through: uma queda no meio deixa o save antigo). Uma escrita por vez; toda
  leitura da pasta espera a anterior (`writer_wait`), e o `atexit` também. Steam Cloud (Project Settings > Online)
  recebe o mesmo arquivo no thread do jogo, e é lido primeiro quando ligado (como o Save Game to Slot já fazia).
- **Auto save**: **Project Settings > Auto Save Interval** (s; 0 desligado), **Auto Save Slots** (1-5, em revezamento:
  AutoSave_1..N), **Auto Save on Level Change** (depois do BeginPlay do novo nível); o nó **Auto Save**, **Set Auto Save
  Interval** (até o Play parar). Nunca com o jogador morto nem no meio de um load; a opção do jogador **AutoSave**
  (Game User Settings, WBP_Options > aba Idioma) desliga o por tempo e o da troca de nível — os Checkpoints continuam.
- **Checkpoint** (`CCheckpoint` + Transform; Place Actors > Checkpoint): uma caixa (extent × escala, girada pelo yaw).
  O pawn do jogador entrando (borda: estava fora): vira o ponto de volta, dispara **On Checkpoint Reached** em todos os
  Blueprints e, com Save When Reached, salva no slot dele ("Checkpoint" por padrão). Only Once: a lista de alcançados
  vai no save. Respawn on Death: a Vida do jogador em 0 → depois do Respawn Delay, recarrega o save do checkpoint (o
  mundo como estava) ou, sem save, move para o Spawn Offset (pés) e Revive. Depois de um load, quem já está dentro de
  um checkpoint não conta como "entrou" (senão ele salvaria de novo na hora).
- **WBP_SaveSlots** (`save_menu.c`, criado em Content/UI quando falta): Continuar, Novo Save (modo salvar), Novo Jogo,
  Voltar e 8 linhas (foto, nome, "nível - data - tempo - tipo", Carregar/Sobrescrever, Apagar) numa Scroll Box. Os
  widgets são ligados por nome (`bind_setting` "Save.*", lido pelo `ui_bind_read` quando a lista muda —
  `save_menu_serial` — e o clique de um Button ligado faz a ação no `ui_event`); o Event Graph tem o OnBack (Esc / B)
  → Remove from Parent. O WBP_PauseMenu ganha **Salvar Jogo / Carregar Jogo** depois de Opções (um menu antigo recebe os
  dois botões e os nós Open Save Slots Menu; um que já tem SaveButton fica como está). Novo Jogo a partir do Main Menu
  Map abre o Game Default Map; de outro nível, o mesmo nível.

## O formato (`.sav`, little-endian)
- Cabeçalho (256 bytes): `RESLOT\r\n` (uma cópia em modo texto quebra), versão (2), tamanho do cabeçalho, kind
  (0 progresso, 1 objeto), flags (auto, checkpoint), **checksum FNV-1a 64** do arquivo inteiro com os próprios 8 bytes
  como zero, bytes do payload, atores, data (unix), tempo de jogo (double), foto (l×a e o offset), nome (64), nível (128).
- Chunks `tag, tamanho, bytes`, encadeados até o fim exato: PROG (tempo de jogo, checkpoint, alcançados), SGOB (classe +
  variáveis do Save Game), ACTR (registros: chave, classe, partes, tamanho; transform 36 B, controle 8 B, Vida 20 B,
  oculto 4 B, Inventário e variáveis com tamanho), DEST (chaves), VARS (Save Game to Slot), THMB (a foto, RGB).
- **Recusado** (Success falso, nada muda, uma linha no log): curto, magic errado, versão maior que a nossa, tamanho ou
  checksum que não batem, chunk que passa do fim. Toda leitura é com limite (nunca confia no arquivo).
- **Migração**: um chunk desconhecido é pulado (um save de uma engine mais nova sem mudar a versão); a versão 1 (o
  arquivo antigo do Save Game to Slot: `RESAV001` + as variáveis) vira na memória uma versão 2 kind Object com um VARS
  igual ao arquivo. Mudou o formato de um chunk? Suba `SAVE_VERSION` e leia as antigas no `apply_*`.

## Nós (21) e eventos
Create Save Game Object, Get / Set Progress Save Game, Save Progress, Load Progress, New Game, Auto Save, Set Auto Save
Interval, List Save Slots, Get Save Slot Info, Get Save Slot Name, Get Latest Save Slot, Get Play Time, Get Current Save
Slot, Respawn at Checkpoint, Get Last Checkpoint, Set Checkpoint, Open Save Slots Menu; eventos On Game Saved (Slot,
Is Auto Save), On Game Loaded (Slot), On Checkpoint Reached (Checkpoint, Player) — nos scripts OnGameSaved,
OnGameLoaded, OnCheckpointReached. Continuam Save Game to Slot, Load Game from Slot, Does Save Game Exist, Delete Game in
Slot (`online.c` só repassa para o módulo).

## Medidas (2026-11-08, RTX 3060, 2560×1417, teste `save_progress`, 1005 atores salvos: 1000 Saveables com Vida + 3 baús Blueprint com Inventário, um cubo, o jogador)
| | ms |
|---|---|
| Save Progress no quadro: o mundo no buffer | 0,13–0,15 |
| ... checksum, cópia e entrega à thread | 0,26 |
| ... a escrita na thread (132 KB, tmp + troca write-through; o jogo não espera) | 1,7–3,3 |
| Foto: espera da GPU pelo quadro + cópia e redução (uma vez por save) | 2–6 + 2,0–2,6 (antes ~1000: memória não-cacheada) |
| Load Progress: leitura + checksum + aplicar | 0,53–0,60 (aplicar 0,24–0,32) |
| ... com o Open Level do nível (1010 atores) e tudo de volta | 10,5–12 |
| `progress_update` por quadro (checkpoints, auto save, tempo) | 0,002 |
| Cena do teste depois dos saves/loads (GPU livre) | 337–542 fps |
Com a GPU dividida com outras sessões os fps do teste caem (6–78 fps visto) — repita isolado.

## Testes
`save_progress` (`--save-test`, projeto novo SAVE, `REALENGINE_SAVE_DIR=build/save_test/SaveGames`): o self-test do
formato (checksum com 3 bytes trocados, cortado, lixo, versão 9, versão 1 migrada, chunk desconhecido pulado, lista,
apagar, nome de slot com `../`), depois jogado: salvar → mudar → carregar (Open Level) → conferir posição, Vida,
Inventário, variável SaveGame (e a comum no padrão), baú destruído antes (some) e depois (volta), o criado no jogo,
Coins do Save Game, On Game Loaded; Checkpoint (save, evento) e a morte → renasce nele vivo; auto save por tempo;
slot danificado recusado sem abrir nível; save versão 1 aberto pelo Load Game from Slot; Save Game to Slot versão 2;
WBP_SaveSlots (linhas, foto, modo salvar, Novo Save, Voltar) e os botões do menu de pausa; custos com limites (save
< 10 ms, aplicar < 20 ms, update < 0,2 ms) e `--min-fps 200`.

## Ainda não
- Variáveis em contêiner (arrays, mapas) e referências a atores não entram (o mesmo limite do Save Game to Slot:
  `portable_var`); físicas: só o transform (a velocidade de um corpo não).
- O checkpoint usa só o yaw da caixa (o editor desenha com a rotação inteira).
- Steam Cloud: a lista de slots é a da pasta local (o Steam não lista aqui); um slot só da nuvem abre pelo nome.
