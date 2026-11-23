# IA (`src/ai.c`, `src/ai_editor.c`)

Dois sistemas que dividem tudo por baixo (Blackboard, percepção, navegação, ações) e conversam entre si.

## Componente **AI** (Detalhes > Adicionar > IA)
O AI Controller + AI Perception da Unreal num componente do pawn: modo (Behavior Tree ou Real Brain) e o asset,
**raio de visão**, **ângulo de visão**, **audição**, **memória** (quanto tempo lembra de um alvo que não vê) e **time**
(0 ignora todos; times diferentes e o jogador são alvos). Um Character com IA anda pela própria Character Movement
(a animação vem junto pela velocidade); outros pawns pelo Floating Movement ou movidos direto.

## NavMesh (2026-11-08: `src/navmesh.c`, `src/navmesh_build.c`; teste `navmesh`)
A navegação da Unreal (Recast/Detour), escrita aqui a partir dos mesmos algoritmos. É o que a IA usa; a grade 2.5D
(abaixo) virou a reserva: enquanto o NavMesh é construído, num Mundo Procedural (o rebase da origem e o streaming de
blocos o refariam inteiro) e se uma consulta não acha o NavMesh perto do agente.
- **Entrada**: o que bloqueia o Character, igual às caixas do `bp_vm.c` (a malha de cada ator sem física simulada, sem
  Colisão Desligada, as Collision que bloqueiam; não: Characters, pawns de IA, projéteis, veículos, escondidos no jogo)
  como caixas alinhadas (as do Character), mais o **Landscape** amostrado a cada 2 células e rasterizado em triângulos
  (com o filtro de inclinação). Formas reais (convexo, malha) entram quando o Character as usar.
- **Por tile** (96 × 96 células de 12 cm = 11,5 m; células maiores se a área passar de ~24 M células; 6 células de borda):
  voxels (spans) → filtros do agente (obstáculo baixo, borda com queda > degrau, pouco espaço acima) → campo compacto
  com vizinhos → Volumes Nula marcados → **erosão pelo raio** → áreas de custo → **regiões monótonas** (sem buracos; ilhas
  < 64 células longe da borda saem) → **contornos** (caminhada, simplificação 1,3 célula, paredes partidas a 12 m) →
  triangulação por orelhas → **polígonos convexos** de até 6 lados → adjacência no tile e portais nos lados → **detalhe
  de altura**: uma grade por polígono (a cada 2 células, o chão da região dele; degraus certos na consulta e no desenho).
  O agente: o maior Character de IA do nível (raio, altura; sem nenhum 34 × 176 cm), degrau 45 cm (o do Character),
  inclinação 45°.
- **Tiles ligados**: cada aresta de portal acha no vizinho as arestas do lado oposto que se sobrepõem (as faixas com a
  altura, como o `connectExtLinks` do Detour), com o pedaço do portal em bytes.
- **Onde**: dentro dos **Nav Mesh Bounds Volume** (Adicionar > Navegação; a caixa do ator, 100 cm × escala); sem volume,
  o nível inteiro até 300 m; maior que isso, 120 m em volta de cada agente de IA (revisto a cada segundo jogando; o que já
  foi feito fica). Ativo só com agentes de IA, volume, P ou Construir Caminhos: sem isso, custo zero.
- **Mudanças**: o anel de mudanças do ECS (`w->changes`; a revisão: tudo de novo, só as diferenças contam). Uma caixa
  que mexeu, nasceu ou sumiu suja os tiles do lugar antigo e do novo. No editor, qualquer ator; **jogando, só Static e
  Stationary** (um Movable conta onde o Play o achou; destruído, sai). No editor espera 0,25 s sem mudança (arrastar um
  ator refaz uma vez, no fim).
- **Em segundo plano**: lotes de até 64 tiles (os mais perto dos agentes primeiro). A thread principal copia a entrada
  (as caixas de cada tile, as alturas do Landscape: o worker não lê o mundo) e põe o resultado no lugar (ligações); o
  worker faz os tiles. Editor: `async_run` (cartão "Construindo a navegação" só se passar de 0,4 s); jogo empacotado e
  `navmesh_wait`: uma thread própria. Um lote de antes de um reset (outro nível) é descartado.
- **Consultas**: A* nos polígonos (nó no meio do portal, custo × a área, heurística 0,999) com até 8192 nós; sem chegar,
  o caminho parcial até o mais perto. Depois o **funil** (string pulling) trecho a trecho, cortado nos Nav Links.
  **Projetar** (o polígono mais perto na caixa; dentro dele, a altura do detalhe), **ponto aleatório alcançável**
  (Dijkstra até o raio, polígono sorteado pela área) e **comprimento do caminho**.
- **Nav Link Proxy** (dois pontos no espaço do ator, nos dois sentidos ou um, raio de encaixe 60 cm): as pontas
  encaixam no NavMesh e viram ligações dos polígonos; no caminho, o ponto de saída leva `NAV_PT_LINK` (+ `JUMP` quando
  não é só descer). O Character **pula**: sobe o bastante para passar a borda (+60 cm), atravessa no máximo na sua
  velocidade de andar (o controle no ar mantém), aterrissa na outra ponta; caiu longe: caminho de novo. Descer de uma
  borda é só andar.
- **Nav Modifier Volume**: Padrão, **Obstáculo** (custo 8), **Água Rasa** (3), **Nula** (marcada antes da erosão: o agente
  fica a um raio dela).
- **Usuários**: AI Move To, Move To e Find Random Location da BT, Wander/Patrol/Flee/Investigate do Brain, a previsão e as
  vagas do cerco (projetadas no NavMesh). Nós novos: **Project Point to Navigation**, **Get Random Reachable Point in
  Radius**, **Get Path Length** (com a grade de reserva). Assistente: artigo `navmesh`.
- **Editor**: **P** mostra o NavMesh (verde por cima, os tiles em dois tons, as paredes claras, só perto da câmera e
  nunca visto por baixo); os Nav Links sempre como arcos (verde ligado, vermelho com uma ponta fora). Path Tracing foi
  para Shift+P. Mostrar > Navegação (P), Compilar > Construir Caminhos, Details > Navegação. Debug Viewer: lista
  "Navigation (NavMesh)".
- Atenção nos testes: esperar só o NavMesh (`navmesh_wait`, a thread própria). `async_wait_all` antes do primeiro quadro
  rodava passos de GPU de outros sistemas e dava 6 erros de validação da swapchain (imagem não adquirida).

### Medidas do NavMesh (i7-11700F, RTX 3060, 1440p)
| O quê | Resultado |
|---|---|
| Teste `navmesh` (chão de 60 m, escada, rampa, vão com Nav Link, corredor e fresta) | 36 tiles, 104 polígonos, 1 lote: 23 ms no worker (o tile mais lento ~1 ms); pôr no lugar 0,03 ms e copiar a entrada 0,01 ms na thread principal; 572-579 fps |
| Tile denso (11,5 m, 400 caixas sobre Landscape ondulado), só o construtor | 2,95 ms por tile, uma thread |
| Um ator Static movido 3 m (no editor ou jogando) | 2 tiles refeitos num lote, 0,82 ms no worker; um Movable jogando: nenhum |
| Caminho (A* + funil), os 4 do teste | 0,003 ms em média; consultas no geral 1,5 µs |
| `--scene ai` (casa) | 4 tiles, 27 polígonos, 1,1 ms no worker (o tile mais lento 0,31 ms); 22 938 consultas em 6 s (a previsão projeta a cada quadro), média 1,9 µs, máx. 0,041 ms |
| Nível sem IA nem volume | nada construído, nenhum custo por quadro (só a checagem dos arquétipos) |

## Navegação (grade 2.5D: a reserva)
`ai_nav_build` no começo do Play: as caixas sólidas do nível viram "spans" por célula (25 cm; maior em níveis grandes,
até ~2M células); o chão de cada célula é o topo mais baixo com 170 cm livres acima; células junto a paredes (vizinho
alto demais ou faltando) saem — a largura de quem anda. A* com 8 vizinhos (sem cortar quinas, degraus ≤ 45 cm) e
**string pulling** (só os cantos que uma linha reta não pula). Teste da casa: 1145 células andáveis em < 0,1 ms.

## Percepção
6 vezes por segundo (escalonado): visão (raio, cone, linha de visada pelas caixas do jogo) e audição (**Report Noise
Event** no Blueprint; os **passos** das animações — notify `Footstep` — são ruídos automaticamente). Escreve no
Blackboard: `TargetActor`, `CanSeeTarget`, `LastKnownLocation`, `DistanceToTarget`, `HeardNoise`, `NoiseLocation`,
`SelfLocation`.

## 1. Behavior Tree (a da Unreal) — `.rbt`
- Editor: Blackboard à esquerda (chaves bool/float/vector/actor), árvore de cima para baixo no centro (filhos da
  esquerda para a direita; clique direito adiciona), Detalhes à direita.
- Compostos: **Selector**, **Sequence**. Tarefas: **Move To** (chave vetor ou ator, raio aceitável; segue um ator
  que se move), **Wait** (± desvio), **Rotate to face BB entry**, **Find Random Location** (no grid), **Set/Clear
  Blackboard Value**, **Run Event** (evento do Blueprint do pawn), **Play Montage**.
- Decoradores: **Blackboard** (Is Set / Is Not Set / > / < / ==, com *observer aborts*: muda → volta ao ramo mais
  importante), **Cooldown**, **Loop**, **Time Limit**. Serviços: **Default Focus**, **Set Movement Speed**.
- Exemplo: `Content/AI/BT_Chase` (vê alguém → corre atrás; senão anda por aí).

## 2. Real Brain (sistema próprio, IA por utilidade) — `.rbrain`
Baseado no que os jogos atuais usam (necessidades de The Sims, Infinite Axis Utility System): cada **comportamento**
tem um peso e até 4 **considerações** (entrada → faixa → curva: Linear, Inversa, Degrau, Suave, Sino, inverter).
Entradas: distância ao alvo, vê o alvo, tempo desde que viu, ouviu barulho, float do Blackboard, **variável do
Blueprint do pawn** (ex.: Health), aleatório, tempo no comportamento, tem alvo. Nota = peso × produto das curvas com
**compensação** (muitas considerações não afogam um bom comportamento); o que está rodando ganha **inércia** (sem
ficar trocando). Ações: Idle, Wander, Chase, Flee, Keep Distance, Investigate (ruído ou último local), Patrol,
Attack (chama o evento, com alcance e recarga), **Run Behavior Tree** (a ponte com o sistema 1), Blueprint Event.
O editor desenha cada curva e, jogando, mostra a **nota ao vivo** de cada comportamento (barra verde = o que roda).
Exemplo: `Content/AI/Brain_Guard` (patrulha, persegue, investiga o último local, confere barulhos, descansa).

## IA avançada: prever, cortar caminho, cercar em esquadrão
Variáveis de dificuldade no componente (seção "Dificuldade e Trabalho em Equipe", botões Fácil/Normal/Difícil):
**Previsão** (s à frente), **Tempo de Reação** (s vendo antes de reagir), **Coordenação** (0-1, parte do esquadrão
que dá a volta), **Raio de Comunicação** (o que um vê, o time dentro do raio sabe), **Raio do Cerco**.
- Velocidade do alvo estimada pelo que o agente vê (média entre olhadas); **interceptação**: a menor raiz de
  |r + v t| = s t (s: a velocidade do próprio agente), limitada à Previsão, projetada na grade. O A* até esse ponto é
  o "atalho" que corta o alvo. Chaves novas de percepção: `PredictedLocation`, `FlankLocation` (entram também nos
  assets antigos ao abrir).
- **Esquadrão** = agentes do mesmo time atrás do mesmo alvo. A cada quadro (`teamwork`): ordenados por ângulo em volta
  do alvo, as vagas do anel nessa ordem (vizinhos continuam vizinhos, ninguém cruza), giradas pela média circular para
  caber onde já estão; os mais próximos (1 − Coordenação) atacam direto, os outros vão primeiro ao flanco.
- Nós novos: BT **Predict Target Location** e **Find Surround Spot**; Brain: ações **Intercept** e **Surround**,
  entradas **Squad Size** e **Target Speed**; Chase usa a previsão e o flanco; Attack em esquadrão ataca pelo seu lado
  do anel. Exemplo pronto: **Brain_Squad** (Search → Intercept > 9 m → Surround < 9 m → Attack < 4,2 m).
- Depuração (tecla '): cruz = onde acha que o alvo vai estar; círculo = sua vaga no anel (cheio: está flanqueando).
- Correção junto: a malha de um ator com Projectile Movement não entra mais nas caixas de colisão (ele parava onde
  nascia).

## Blueprints
Get/Set Blackboard Value as Float/Bool/Vector/Actor (a chave nos Detalhes do nó), **AI Move To**, **Run Behavior
Tree**, **Report Noise Event**; as ações chamam eventos do Blueprint (Run Event, Attack, Blueprint Event).

## Depuração
- Viewport jogando: tecla **'** (ou Mostrar > Depuração de IA) — caminho de cada agente, cone de visão (verde quando
  vê), o que está fazendo acima da cabeça (o gameplay debugger da Unreal).
- Debug Viewer: lista **AI** (tempo do quadro, caminhos buscados, o que cada agente faz).

## Medidas (RTX 3060, 1440p)
| Cena | Resultado |
|---|---|
| `--scene ai` (Hunter com BT_Chase + 2 Guards com Brain_Guard na casa) | 518 fps; IA 0,001 ms/quadro; grade em < 0,1 ms |
| `--scene ai_squad` (4 caçadores Brain_Squad, um alvo correndo a 260 cm/s) | 412-460 fps; IA 0,003-0,009 ms/quadro; em 4-9 s os 4 sabem (1 só por comunicação), 2 flanqueiam, distância média ~3,6 m, maior vão entre eles 112-118° |
Teste `ai` (os três agentes e a navegação existem, ≥ 300 fps). Teste `ai_squad` (os 4 sabem, ≥ 2 miram à frente,
distância média < 7 m, vão < 180°, ≥ 300 fps).

## Sentido de dano e desvio entre agentes (Q24, 2026-11-18)
- **Sentido de dano** (o AISense_Damage da Unreal): `ai_report_damage`, chamado pelo `health_apply_damage`. O agente
  atingido sabe na hora quem bateu (o Instigator), de qualquer lado, sem cone e sem tempo de reação: alvo, último lugar
  (seguido 1,5 s), barulho no lugar (4 s); parado, vira para o atirador por 2 s enquanto não o vê. Fogo amigo e time 0
  não contam. Antes um inimigo atingido pelas costas não reagia.
- **Desvio local** (tipo o RVO da Unreal), ligado em todo agente que anda por Character ou Floating Movement: um hash de
  1024 células de 3 m por quadro (agentes + o pawn do jogador, arrays estáticos); quem vai encontrar outro em até 1,2 s
  se desvia (para longe do ponto do encontro; de frente, cada um para a sua direita); encostados, se afastam. O alvo e o
  ator do Move To não contam. `REALENGINE_AI_AVOID=0` desliga; `ai_stats()->avoiding` conta os que desviaram no quadro.
- Teste `ai_senses` (`ai_senses_test.c`): Victim atingida pelas costas sabe e vê o Shooter em < 2 s, Control não sabe;
  dois Characters de frente passam sem se atravessar (menor distância ≥ 80% dos dois raios) e chegam.
- Medidas e o resto da auditoria (State Tree, EQS, Smart Objects, Visual Logger...): [auditoria-ia-programacao.md](auditoria-ia-programacao.md).

## Limites / próximos passos
- NavMesh: regiões monótonas (polígonos às vezes compridos; sem o watershed da Unreal), um tipo de agente só, Nav Links
  só de andar/pular (sem os Smart Links com evento), desvio local simples (sem ORCA nem filas: Q24.6), sem EQS; mundos
  procedurais na grade. O plano está em [auditoria-ia-programacao.md](auditoria-ia-programacao.md).
- Times/afiliação simples; dano e "Health" vêm de variáveis de Blueprint.

## Perder o alvo menos (2026-11-02)
Posição do alvo copiada todo frame enquanto visível e por 1,5 s depois (os passos dizem para onde foi); visto há < 3 s, é
sentido até 4,5 m fora do cone; no último trecho o caminho vai direto à posição atual; repath do alvo móvel 0,25 s.
