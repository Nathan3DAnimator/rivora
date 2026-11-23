# Online: multiplayer, Steam e saves (2026-11-04)

Pedido: "um sistema de multiplayer para o dev conseguir fazer jogos com online e multiplayer, sistemas que conectem com a
Steam, usando a Steam API, mas sempre da melhor forma possível para resolver lag e packet loss; aproveitar salvamento em
nuvem, conquistas e tudo o que a Steam oferece; completo, o desenvolvedor com controle sobre tudo, intuitivo, com bastante
nodes e interfaces". Escolha do usuário: a **Steam carregada em tempo de execução** (a engine compila sem o SDK).

## Camadas
| Arquivo | O quê |
|---|---|
| `net.h/.c` | Protocolo próprio sobre datagramas: conexões, acks, 3 canais, medidas, emulador de rede |
| `net_udp.c` | Transportes: UDP (Winsock, descoberta na LAN por broadcast) e loopback em processo (testes) |
| `steam.h/.c` | `steam_api64.dll` pela API flat: usuário, amigos, overlay, presença, lobbies, P2P pelo relay, Cloud, conquistas, stats, placares, DLC, idioma |
| `online.h/.c` | O jogo: sessões, jogadores e pawns, replicação, RPCs, variáveis replicadas, chat, viagem de nível, saves |
| `bp_vm.c`, `blueprint.c`, `bp_editor.c` | 101 nós de Blueprint (Online, Advanced Sessions, Steam, Save Game) |
| `net_test.c`, `net_session_test.c` | Testes: protocolo (`--net-test`), sessão com dois processos (`--net-host-test`, `--net-adv`), Steam (`--steam-test`) |

## O protocolo (`net.c`): lag e perda de pacotes
- Tudo sobre datagramas não confiáveis, igual em UDP, Steam (ISteamNetworkingMessages, sem a confiabilidade dela) e loopback.
- Cada pacote confirma os últimos 33 recebidos (sequência + campo de 32 bits): sem pacotes só de ack; quando chegou dado e
  não há o que mandar, sai um ack no próximo flush (o RTT honesto). Um pacote só leva acks depois que o primeiro chegou (um
  "ack 0" inicial confirmava um pacote perdido e travava o canal confiável — achado pelo teste).
- **RELIABLE**: ordenado e garantido; reenvio pelo RTT (1,25×RTT + 2×jitter, ≥ 30 ms); só manda dentro da janela de espera
  do receptor a partir da mais velha não confirmada (uma mensagem além dela seria descartada lá com o pacote confirmado).
- **UNRELIABLE**: o estado mais novo (nunca reenviado). **SEQUENCED**: idem, e uma mais velha que a última entregue é descartada.
- Medidas por conexão: RTT suavizado, jitter, perda (cada pacote julgado após 3 RTT: confirmado ou perdido), kbps.
- **Emulador** em qualquer transporte: latência, jitter, perda, duplicação (Project Settings > Online > Emulação de Rede,
  aplicada no Play do editor; nó Set Network Emulation; `--net-emu` nos testes).
- Sem malloc por quadro: anéis fixos por par, mensagens empacotadas em pacotes de até 1200 bytes.

## O jogo (`online.c`)
- **Autoridade no servidor** (listen: um jogador hospeda; dedicado: `realengine_game --server`). O pawn do próprio cliente
  anda na hora (sem atraso de input) e manda seu estado a 30 Hz; o servidor confere a velocidade e corrige um salto
  impossível (trapaça, bug) com `M_CORRECT`.
- **Replicação**: todo Character, todo corpo com Simulate Physics e o que for marcado (Set Replicates). Snapshots
  quantizados (posição 1/8 cm em 3×24 bits, ângulos 16 bits, velocidade 4 cm/s), só o que mudou (e tudo uma vez por
  segundo), cortados pela distância de relevância e pelo orçamento de banda por cliente.
- **Interpolação nos clientes**: desenhados no passado, `max(atraso do projeto, 2 ticks + 2×jitter)`; posição por Hermite
  com as velocidades, ângulos pelo caminho curto; faltou snapshot: extrapola pela velocidade até 250 ms e segura.
- **RPCs** (Run on Server, Multicast, Run on Owning Client), confiáveis ou não, com um float, um vetor e um texto (Get RPC
  ...); **variáveis replicadas** (Replicate Variable: o valor do servidor a todos, depois `OnRep_<Nome>` como o RepNotify);
  **chat**; **spawn/destroy replicados**; **viagem de nível** (Open Level no servidor leva os clientes, como o ServerTravel).
- Atores do nível se casam pelo nome nos dois lados (o mesmo nível); os spawnados pela classe.
- **Sessões**: LAN (UDP 7777, achadas por broadcast com nome, jogadores e host) ou Steam (lobby público com `re_version`,
  P2P pelo relay da Valve: sem abrir portas, IP escondido; convites pelo overlay entram no lobby e conectam no dono).
- **Saves** (Save/Load Game to Slot): as variáveis simples do ator (números, vetores, textos) em `Saved/SaveGames/<slot>.sav`,
  e na Steam Cloud quando ligado (lida primeiro). Desde o R7 módulo 4 o arquivo (versão, checksum, metadados) e o
  progresso inteiro moram em `save_progress.c` (notas-dev/salvar-progresso.md); `online_save/load` só repassam.
- **Regras de jogo** (R7 módulo 8, notas-dev/regras-de-jogo.md): a mensagem `M_RULES` (confiável, servidor → clientes)
  leva a partida do Game Rules — fase, rodada, cronômetro, pontos, vidas — quando muda e a cada segundo (87 bytes com 2
  jogadores); o cliente dispara os mesmos eventos e não decide nada.
- Debug Viewer (F10) > **Online**: CPU da rede por quadro, cada jogador com RTT, perda e banda; interpolação e correções.

## Steam (`steam.c`)
- `LoadLibrary("steam_api64.dll")` (o jogo leva a do SDK do Steamworks ao lado do .exe). Os acessores das interfaces levam a
  versão no nome (`SteamAPI_SteamUser_v023`): procurados de v030 para baixo, então DLLs de SDKs diferentes servem.
  `SteamAPI_InitFlat` (SDK ≥ 1.58) ou `SteamAPI_Init`. Fora da Steam o App ID vai para `steam_appid.txt`.
- Callbacks despachados à mão (`SteamAPI_ManualDispatch_*`) uma vez por quadro, lidos nos offsets dos headers da Valve
  (empacotamento 8 no Windows); os resultados assíncronos (lobby criado, lista, placares) pelo `SteamAPICallCompleted_t`.
- Sem a DLL ou sem a Steam aberta tudo responde "indisponível" e o jogo segue (rede por UDP, saves no disco).

## Nós de Blueprint (101)
- **Online|Session**: Create Session, Find Sessions, Get Session Count / Name / Players, Join Session, Join Address, Destroy
  Session, Is Server, Is Client, Is In Session.
- **Online|Players**: Get Player Count / Name / Pawn, Get Ping, Get Packet Loss, Get Local Player Index, Kick Player, Send
  Chat Message.
- **Online|Replication**: Has Authority, Is Locally Controlled, Set Replicates, Run on Server, Multicast, Run on Owning
  Client, Get RPC Float / Vector / Text / Sender, Replicate Variable, Spawn Actor (Replicated), Destroy Actor (Replicated).
- **Online|Debug**: Set Network Emulation. **Online|Events**: On Player Joined / Left, On Session Created, On Sessions
  Found, On Join Session Complete, On Network Error, On Chat Message.
- **Steam**: Is Steam Available, Get Steam Name / ID, Unlock / Clear Achievement, Is Achievement Unlocked, Show Achievement
  Progress, Set / Get Stat, Store Stats, Upload / Download Leaderboard, Get Leaderboard Entry Count / Name / Score / Rank,
  Open Steam Overlay, Invite Friends, Set Rich Presence, Is DLC Installed, Get Steam Language, Get Friend Count / Name, Is
  Steam Cloud Enabled; eventos On Leaderboard Downloaded, On Steam Overlay.
- **Game|Save Game**: Save Game to Slot, Load Game from Slot, Does Save Game Exist, Delete Game in Slot.
- **Online|Advanced Sessions** (34, seção abaixo).

## Sessões avançadas (2026-11-07, pedido do usuário: "trazer o Advanced Sessions")
Pedido: o plugin **Advanced Sessions** da Unreal, com mais opções para o desenvolvedor controlar a criação, a busca e a
entrada nas sessões. Nomes dos nós iguais aos do plugin onde ele tem.
- **Configurações** (`OnlineSessionSettings`, o `FOnlineSessionSettings` da Unreal): vagas públicas e **privadas** (só
  para quem vem por convite, sessão de amigo ou endereço), LAN/Steam, **visibilidade** (pública, só amigos, só convite),
  **senha**, Should Advertise, Allow Join In Progress, Allow Invites, joinable (trancar), mapa, modo de jogo, build (nome
  + versão do projeto) e até **16 propriedades** próprias (as Extra Settings: chave e valor em texto).
- **Estado** (Get Session State): nenhum, criando, pendente (o lobby), em andamento (Start Session), encerrada (End
  Session). Sem Allow Join In Progress ninguém entra depois do Start.
- **Busca avançada** (`OnlineSearch`): tipo de servidor (todos, listen, dedicado), Max Results, vagas mínimas, sem
  cheias, só vazias / só com gente (sem contar o host de um listen), sem senha, sem começadas, ping máximo, mesma
  versão, e até **8 filtros por propriedade** (`=`, `!=`, `<`, `<=`, `>`, `>=`, contém; números como números; as chaves
  `name`, `map`, `game_mode`, `host` leem os campos da sessão). Os filtros valem igual na LAN e na Steam: na Steam
  também vão como filtros do lobby (numérico ou texto, vagas livres), e todos são conferidos de novo nos resultados.
  Resultados do mais perto ao mais longe, depois os mais cheios. Sessões trancadas ou começadas sem join in progress não
  aparecem (entrar só falharia).
- **Onde as configurações aparecem**:
  - LAN: a resposta da busca ganhou a versão 2 depois dos campos antigos (um jogo velho lê só o começo): id da sessão,
    flags, vagas privadas, build, mapa, modo, propriedades (até caberem em 512 bytes). Sessão não anunciada (Should
    Advertise desligado ou só convite) não responde. A busca pergunta também a `127.0.0.1` (duas instâncias no mesmo PC,
    sem placa de rede ou com firewall); a mesma sessão respondendo duas vezes vira uma (pelo id). O ping é o tempo da
    resposta.
  - Steam: dados do lobby `re_*` (build, mapa, modo, senha sim/não, estado, join in progress, dedicado, vagas privadas,
    id, máximo) e `p_<chave>` por propriedade, com as chaves em `re_props`. Tipo, limite de membros e joinable do lobby
    seguem as configurações. A Steam não dá ping do lobby (-1).
  - Clientes: mensagem nova `M_SESSION` (confiável) na entrada e a cada mudança: a cópia deles (Get Current Session
    Settings / Property) e os eventos On Session Updated / On Session State Changed. A senha nunca vai (só "tem senha").
- **Entrada**: o HELLO do cliente leva o hash da senha (FNV-1a 64, nunca o texto), o build e se veio por convite/endereço
  (`ONLINE_PROTOCOL` subiu para 2). O servidor recusa com o motivo: banido, outra versão, senha errada / sem senha,
  trancada, partida começada, cheia (as privadas só para convite/endereço). A recusa vai como `M_KICK` antes de fechar,
  então o cliente recebe **On Join Session Complete** com o motivo (e Get Last Session Error); o host recebe **On Join
  Refused**. O transporte aceita 2 conexões além das vagas para poder dizer o motivo. O que a busca já sabe (cheia,
  trancada, senha faltando, outra versão) falha antes de sair da máquina.
- **Banir** (Ban Player): expulsa e guarda Steam ID (ou o endereço, sem Steam); Unban, Unban All, a lista. Get Player
  Unique ID: `steam:<id>` ou `lan:<nome>`.
- **Amigos e convites (Steam)**: Is Friend In Session e Join Friend Session pelo `GetFriendGamePlayed` (o lobby do
  amigo neste jogo), Invite Friend To Session; Set Auto Accept Invites (desligado: só o evento On Session Invite
  Accepted, e Accept Session Invite entra quando o jogo quiser).
- Limites conhecidos: as vagas privadas e o "veio por convite" são declarados pelo cliente (reserva de vagas, não
  segurança); convidados também precisam da senha; na Steam sem ping.

### Nós (34)
- Criar/buscar/entrar: Create Advanced Session, Find Sessions Advanced, Add Session Search Filter, Clear Session Search
  Filters, Get Session Details (12 saídas), Get Session Property, Join Session Advanced, Join Address Advanced, Is
  Finding Sessions, Get Last Session Error.
- Sessão aberta: Set / Remove Session Property, Get Current Session Property, Get Current Session Settings (12 saídas),
  Update Session, Start Session, End Session, Set Session Joinable, Get Session State.
- Jogadores: Ban Player, Unban Player, Unban All Players, Get Banned Player Count / Name, Get Player Unique ID.
- Steam: Is Friend In Session, Join Friend Session, Invite Friend To Session, Set Auto Accept Invites, Accept Session
  Invite.
- Eventos: On Session Updated, On Session State Changed, On Session Invite Accepted, On Join Refused.
- Nos scripts saem sozinhos (`CreateAdvancedSession(...)`, `FindSessionsAdvanced(...)`...). Artigo do Assistente
  `advanced-sessions` (receita Blueprint e Real, compilada pelo teste `assistant`).

## Medidas e testes (RTX 3060; os dois processos na mesma máquina)
- `net_protocol` (`--net-test`, tempo virtual, 8 sementes): 150 ms RTT, ±20 ms, 10% de perda e 2% de duplicação de cada
  lado: 3000 mensagens confiáveis entregues em ordem em 3,2–4,0 s em todas, 0 fora de ordem; RTT medido 163–183 ms; perda
  medida 6–11%; sequenciados nunca voltam. Padrão do jogo (servidor 250 Hz, cliente 120 Hz, 30 Hz cada lado, 100 ms,
  ±20 ms, 5%): perda medida 5,2% / 7,5%, 38 / 13 kbps.
- `net_session` (dois processos, LAN, 100 ms + ±20 ms + 5% de perda em cada um): o cliente entra, possui o próprio pawn e vê
  o pawn do host andar 41 m com o maior passo de 14 cm por quadro (sem saltos), RTT 222 ms, 0 correções.
- `--steam-test` (manual: precisa da Steam aberta e de uma `steam_api64.dll`): App ID 480, usuário, idioma, 35 amigos,
  nuvem ligada, conquista lida, P2P pronto — só leitura (nenhum lobby criado, nada gravado).
- CPU da rede por quadro: no Debug Viewer > Online.
- `net_session_advanced` (`--net-host-test --net-adv`, dois processos, LAN, sem emulação): o host cria "AdvTest" com
  senha, mapa, modo, 1 vaga privada e as propriedades region=sa, level=3. O cliente busca com `region = sa` e
  `level >= 2` (acha 1, com nome, host, 1/4 +1, mapa, modo, senha, ping ~1 ms e level certos), com `region = eu` (0),
  entra com senha errada ("wrong password"), com a certa (jogador 1), vê na cópia dele a propriedade nova e o Start
  Session do host, é banido ("banned: test ban") e, voltando, é recusado ("banned from this session"). O host conta 2
  recusas e 1 ban. ~18 s, passa visível e em segundo plano. A saída do cliente agora aparece no log do teste (o
  processo filho herda a saída).
- Custo: nada por quadro. A publicação (resposta da LAN, lobby, `M_SESSION`) só roda quando algo muda; os filtros só
  no fim de uma busca (64 resultados × 8 filtros).

## Predição com reconciliação e vários jogadores no Play (B13, 09/11)
- **Movimento autoritativo do Character** (`ONLINE_PROTOCOL` 4): o tick do bp_vm chama um gancho depois de mover o pawn
  possuído (`bp_set_move_hook`): o cliente numera o movimento (entrada xy, pulo, dt) e guarda onde ele levou (anel de
  256). A cada tick de rede manda `M_MOVE` (não confiável sequenciado) com os movimentos ainda não confirmados (até 40;
  dt em 0,1 ms, entrada em int8, pulo 1 byte: 5 bytes cada). O servidor aplica os novos com `bp_character_step` (o
  mesmo `move_character`, pulo como o nó Jump) e deixa o pawn fora do próprio tick (`bp_character_skip`); responde
  `M_MOVE_ACK` com o último aplicado, posição e velocidade em float exato e se está no chão.
- **Reconciliação**: diferença > 2 cm do estado guardado daquele movimento -> o estado do servidor, depois os movimentos
  seguintes refeitos (e os estados guardados atualizados). O pulo passou a ser contado (`CCharacter.jumps`) para entrar
  no movimento. Pawns que não são Character (veículos, Floating) seguem com `M_OWNER` (estado + checagem de velocidade).
- **Vários jogadores no Play** (setinha do Play > Number of Players, 1-4): o nível do editor salvo em
  `Saved/PIE/PIE_Level.rlevel`, `online_host` em LAN, e N-1 processos `realengine.exe --project ... --open <nível>
  --play --pie-client 127.0.0.1 --pie-window k` (janelas de 1280x720 em cascata) que entram sozinhos; Stop e o fechamento
  do editor terminam os processos (`re_process_start`/`re_process_kill` na plataforma). Um cliente é sempre "automated"
  (não grava as preferências do usuário).
- **Medidas** (`net_session`: 100 ms, jitter 20, 5% de perda): o cliente anda 50 m num quadrado pulando; 1419 movimentos,
  1402 aplicados no servidor, 204 confirmações, 17 correções (537 movimentos refeitos), diferença média 1,51 cm, pior
  64,6 cm (a primeira confirmação, de onde o pawn nasceu). `pie_multiplayer`: 2 de 2 jogadores na sessão.
- **Falta**: suavizar na tela a correção grande (hoje um salto), relevância e prioridade além da distância, servidor
  dedicado headless sem janela, compressão delta campo a campo.

## Falta / próximos passos
- Compressão delta contra o último snapshot confirmado (a base confirmada já existe: falta mandar só a diferença).
- Steam: Workshop (UGC), inventário, voz. (Lobby privado/amigos: feito com as sessões avançadas.)
- Sessões avançadas: ping dos lobbies da Steam (ISteamNetworkingUtils), entrar pela presença (`GameRichPresenceJoinRequested`),
  vagas privadas verificadas pelo servidor (convite real da Steam).

## Lag e perda (2026-11-07, pedido: "minimizar problemas de lag e perda de pacote, jogo mais liso online")
Código: `src/net_repl.h/.c` (novo), `net.c` (`net_send_tracked` / `net_take_acked`), `online.c`. `ONLINE_PROTOCOL` 3.
- **Achado**: o servidor marcava um estado como enviado ao mandar, não ao chegar. Pacote perdido com o ator parado: o
  cliente via o ator no lugar errado até o refresco de 1 s (ou 2 s, se o refresco também se perdia).
- **Base confirmada por cliente**: snapshots no canal não confiável **rastreado** (o token volta quando o pacote é
  confirmado). Por jogador e ator: o último enviado e o último confirmado. Manda se mexeu desde o último envio, ou se o
  cliente não confirmou o que mostra e o envio tem mais de uma ida e volta (1,25×RTT + 2×jitter + 1 tick). Refresco
  lento de 5 s. Um log por mensagem (64 no anel) diz quais atores ela levou. Ordem por ator pelo tempo (o canal não
  descarta mais por sequência: uma mensagem descartada como velha tinha o pacote confirmado).
- **Rodízio**: o snapshot começa onde o orçamento do tick anterior parou (nenhum ator fica sempre por último).
- **Estados compactos**: byte de flags, posição, só o yaw (pitch/roll quando não zero), velocidade só andando, ids de
  2 bytes até 32767: 12 bytes parado, 18 andando (21 antes) + id 2 (4 antes).
- **Cliente**: atraso de desenho `net_repl_delay` = 2 ticks + 1 com perda > 2% + 1 com > 10% + 2×jitter (nunca abaixo
  do projeto), suavizado; **relógio** que acelera/freia até 10% em vez de pular (pula só > 250 ms); o fuso do servidor
  só pelo snapshot mais novo; **salto suavizado**: um desvio maior que o movimento esperado vira deslocamento que some
  em ~0,1 s (teleporte > 3 m vai na hora).
- **Compensação de lag** (`online_lag_trace`, nó **Lag Compensated Trace**, Online|Replication): o servidor guarda 64
  posições por ator (~2 s a 30 Hz); o tiro do jogador volta RTT/2 + o atraso de desenho dele (que o cliente manda no
  `M_OWNER`), até 1 s; cápsula do Character (raio + o do traço), esfera de 50 cm para o resto; o nível no caminho pelo
  `bp_trace_boxes`. No cliente ou sozinho: as posições de agora.

### Medidas (2026-11-07)
- `net_protocol` (`--net-test`): os tokens rastreados nunca voltam de mensagem que não chegou (os "falsos" são só os
  sequenciados descartados como velhos); `net-repl`: 64 atores andando e parando, 20% de perda de pacote e de ack,
  40 sementes: **pior caso 533 ms** para todos estarem no lugar certo, contra **2067 ms** da regra antiga.
- `net_session` em segundo plano: maior passo 8–10 cm, 0 correções. **Visível** (o host sem GPU: ~84 fps com quadros de
  530 ms, RTT ~550 ms): antes falhava (passo de 94–122 cm), agora passa (2,7–9,4 cm; o atraso subiu sozinho para
  285–556 ms).

## Replicação como na Unreal (2026-11-10)
- Variável: Details > **Replication** None / Replicated / RepNotify (`BpVar.rep`, `.rbp` "rep"). O Set da variável
  compila também o `BPK_NET_REPLICATE_VAR`; RepNotify cria o Custom Event `OnRep_<Nome>` e o roda no servidor e no
  standalone (o host listen já rodava).
- Custom Event: Details > **Replicates** Not Replicated / Multicast / Run on Server / Run on owning Client + Reliable
  (`BpNode.net`, `.rbp` "net"). O Call do evento compila como o RPC (`online_rpc`); viajam o 1º Float, Vector e Text
  da chamada (Get RPC Float / Vector / Text do outro lado). O título do nó mostra o modo.
